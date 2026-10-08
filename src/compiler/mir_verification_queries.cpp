#include "mir_verification_queries.h"
#include "gti/mir_dominance.h"
#include "gti/mir_effects.h"
#include "mir_verification_support.h"

#include <algorithm>
#include <climits>
#include <unordered_set>

namespace lang::mir_detail {

// Checked reference extraction transports an external dependency, independently
// of whether its carrier is a binding or a fresh SSA call result. The same
// proof also validates an extracted address stored in a local return alias.
bool fallibleReferenceOwnerTransport(const MirBody &body,
                                     const MirInstruction &instruction,
                                     const MirLoan &extracted) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::ExpectedValue ||
      instruction.functionTarget || instruction.constructorTarget ||
      instruction.lambdaTarget || !instruction.loan ||
      *instruction.loan != extracted.id ||
      instruction.hirValue != extracted.producedBy || !instruction.receiver ||
      instruction.receiver->type.kind != SemanticType::Expected ||
      instruction.receiver->type.arguments.size() != 2)
    return false;
  const auto &reference = instruction.receiver->type.arguments.front();
  const MirPlace *origin = body.findPlace(extracted.source);
  if (reference.kind != SemanticType::Reference ||
      reference.arguments.size() != 1 ||
      reference.referenceAccess != AccessMode::ReadOnly ||
      instruction.info.type != reference.arguments.front() ||
      instruction.borrowAccess != AccessMode::ReadOnly || origin == nullptr ||
      origin->root == MirPlaceRootKind::Value ||
      origin->root == MirPlaceRootKind::Temporary ||
      extracted.source == instruction.receiver->place ||
      extracted.access != AccessMode::ReadOnly ||
      instruction.borrowOrigin != BorrowOriginKind::Receiver)
    return false;
  const auto sameOwner = [&](const MirLoan &dependency) {
    return dependency.id != extracted.id &&
           dependency.access == AccessMode::ReadOnly &&
           sameCanonicalPlace(
               canonicalBorrowOrigin(body, dependency.source, {}, false),
               canonicalBorrowOrigin(body, extracted.source, {}, false));
  };
  std::unordered_set<MirPlaceId> places;
  std::unordered_set<MirValueId> values;
  const auto preservesOwner = [&](const auto &self,
                                  const MirOperand &operand) -> bool {
    MirValueId valueId = operand.value;
    if (operand.kind != MirOperandKind::Value) {
      const MirPlace *carrier = body.findPlace(operand.place);
      if (carrier == nullptr || !places.insert(carrier->id).second)
        return false;
      if (carrier->root == MirPlaceRootKind::Binding)
        return std::any_of(body.loans.begin(), body.loans.end(),
                           [&](const MirLoan &dependency) {
                             return std::find(dependency.carriers.begin(),
                                              dependency.carriers.end(),
                                              carrier->binding) !=
                                        dependency.carriers.end() &&
                                    sameOwner(dependency);
                           });
      if (carrier->root != MirPlaceRootKind::Value)
        return false;
      valueId = carrier->value;
    }
    if (!values.insert(valueId).second)
      return false;
    const MirInstruction *producer = linearDefinition(body, valueId);
    if (producer == nullptr || producer->info.type != operand.type)
      return false;
    if (producer->loan) {
      const MirLoan *dependency = body.findLoan(*producer->loan);
      return dependency != nullptr && sameOwner(*dependency);
    }
    return (producer->kind == MirInstructionKind::Move ||
            producer->kind == MirInstructionKind::Load) &&
           producer->operands.size() == 1 &&
           self(self, producer->operands.front());
  };
  return preservesOwner(preservesOwner, *instruction.receiver);
}

bool referenceParameterReturnPublication(const MirBody &body,
                                         const MirBlock &block,
                                         const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Borrow || !instruction.loan ||
      instruction.operands.size() != 1)
    return false;
  const auto *published = body.findLoan(*instruction.loan);
  if (published == nullptr)
    return false;
  const auto &produced = *published;
  const auto &source = instruction.operands.front();
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
  if (carrier == nullptr || carrier->root != MirPlaceRootKind::Binding ||
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
    const bool selectedAlias = std::any_of(
        body.places.begin(), body.places.end(), [&](const MirPlace &place) {
          return place.root == MirPlaceRootKind::Binding &&
                 place.binding != carrier->binding &&
                 place.sourceValue == instruction.hirValue &&
                 place.type == source.type &&
                 std::find(retained->carriers.begin(), retained->carriers.end(),
                           place.binding) != retained->carriers.end();
        });
    if (selectedAlias ||
        carrier->type.arguments.front() != instruction.info.type)
      return false;
  } else {
    const auto exactRuntimeAlias =
        [&](const auto &self, const MirPlace *runtime,
            const MirBlock &consumerBlock, const MirInstruction &consumer,
            std::size_t depth) -> bool {
      if (depth > body.places.size() || runtime == nullptr ||
          runtime->root != MirPlaceRootKind::Binding ||
          runtime->binding == carrier->binding ||
          (depth == 0 && (runtime->type != source.type ||
                          runtime->sourceValue != instruction.hirValue)) ||
          runtime->projections.empty() ||
          runtime->projections.front().kind != MirProjectionKind::Dereference ||
          !std::all_of(std::next(runtime->projections.begin()),
                       runtime->projections.end(),
                       [](const auto &projection) {
                         return projection.kind == MirProjectionKind::Field &&
                                projection.field != 0 &&
                                projection.index == 0 &&
                                !projection.constantIndex &&
                                projection.selection == 0;
                       }) ||
          (produced.access == AccessMode::Mutable &&
           runtime->access != AccessMode::Mutable) ||
          std::find(retained->carriers.begin(), retained->carriers.end(),
                    runtime->binding) == retained->carriers.end())
        return false;
      const MirInstruction *initialize = nullptr;
      const MirBlock *initializeBlock = nullptr;
      for (const auto &candidateBlock : body.blocks)
        for (const auto &candidate : candidateBlock.instructions) {
          const auto *destination = candidate.destination
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
              destination->symbol != runtime->symbol ||
              (runtime->projections.size() == 1 &&
               destination->type.arguments.front() != runtime->type) ||
              (runtime->access == AccessMode::Mutable &&
               destination->type.referenceAccess != AccessMode::Mutable) ||
              candidate.operands.size() != 1)
            return false;
          initialize = &candidate;
          initializeBlock = &candidateBlock;
        }
      if (initialize == nullptr)
        return false;
      const auto &input = initialize->operands.front();
      const auto *address = body.findPlace(input.place);
      if (input.kind != MirOperandKind::Loan || input.loan != retained->id ||
          input.type != body.findPlace(*initialize->destination)
                            ->type.arguments.front() ||
          input.value != 0 || address == nullptr || address->type != input.type)
        return false;
      // Ordinary verification proves the call result's producer and
      // Initialize address. Its sole publication must dominate this
      // return; carrier metadata or matching types alone grant nothing.
      if (initializeBlock == &consumerBlock) {
        const auto before = std::find_if(
            consumerBlock.instructions.begin(),
            consumerBlock.instructions.end(),
            [&](const auto &candidate) { return &candidate == initialize; });
        const auto after = std::find_if(
            consumerBlock.instructions.begin(),
            consumerBlock.instructions.end(),
            [&](const auto &candidate) { return &candidate == &consumer; });
        if (before >= after)
          return false;
      } else {
        const auto dominance = computeMirDominance(body);
        if (!dominance ||
            !dominance->dominates(initializeBlock->id, consumerBlock.id))
          return false;
      }
      if (address->root == MirPlaceRootKind::Binding) {
        if (address->binding == carrier->binding)
          return mir_detail::referenceParameterContainsPlace(body, *retained,
                                                             address->id) &&
                 (runtime->access == AccessMode::ReadOnly ||
                  address->access == AccessMode::Mutable);
        return self(self, address, *initializeBlock, *initialize, depth + 1);
      }
      const auto *resultLoan = body.findLoan(address->loan);
      const bool extractedAddress = [&] {
        if (resultLoan == nullptr || resultLoan->parent != 0 ||
            resultLoan->access != AccessMode::ReadOnly ||
            produced.access != AccessMode::ReadOnly)
          return false;
        for (const auto &candidateBlock : body.blocks)
          for (const auto &candidate : candidateBlock.instructions)
            if (candidate.loan == resultLoan->id &&
                candidate.info.type == address->type &&
                fallibleReferenceOwnerTransport(body, candidate, *resultLoan))
              return true;
        return false;
      }();
      return address->root == MirPlaceRootKind::Loan &&
             address->projections.empty() && resultLoan != nullptr &&
             resultLoan->kind == MirLoanKind::CallResult &&
             (resultLoan->parent == retained->id || extractedAddress) &&
             (resultLoan->source == retained->source ||
              mir_detail::referenceParameterContainsPlace(
                  body, *retained, resultLoan->source)) &&
             resultLoan->storedDependency == retained->storedDependency &&
             resultLoan->producedBy != 0 &&
             (runtime->access == AccessMode::ReadOnly ||
              resultLoan->access == AccessMode::Mutable);
    };
    if (!exactRuntimeAlias(exactRuntimeAlias, body.findPlace(source.place),
                           block, instruction, 0))
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
         *retire.loan == source.loan && retire.hirValue == instruction.hirValue;
}

bool referenceParameterContainsPlace(const MirBody &body,
                                     const MirLoan &parameter,
                                     MirPlaceId place) {
  const MirPlace *formal = body.findPlace(parameter.source);
  if (parameter.kind != MirLoanKind::Parameter || !parameter.entry ||
      parameter.parent != 0 || parameter.escapes || parameter.producedBy != 0 ||
      formal == nullptr || formal->root != MirPlaceRootKind::Binding ||
      !formal->projections.empty() ||
      formal->type.kind != SemanticType::Reference ||
      formal->type.arguments.size() != 1 ||
      std::find(parameter.carriers.begin(), parameter.carriers.end(),
                formal->binding) == parameter.carriers.end())
    return false;
  const auto contains = [&](const auto &self, MirPlaceId id,
                            std::size_t depth) -> bool {
    const MirPlace *projected = body.findPlace(id);
    if (projected == nullptr || depth > body.places.size() + body.loans.size())
      return false;
    if (id == parameter.source)
      return true;
    if (projected->root != MirPlaceRootKind::Binding ||
        projected->projections.empty() ||
        projected->projections.front() !=
            MirPlaceProjection{.kind = MirProjectionKind::Dereference} ||
        !std::all_of(std::next(projected->projections.begin()),
                     projected->projections.end(), [](const auto &projection) {
                       return projection.kind == MirProjectionKind::Field &&
                              projection.field != 0 && projection.index == 0 &&
                              !projection.constantIndex &&
                              projection.selection == 0;
                     }))
      return false;
    if (projected->binding == formal->binding)
      return projected->symbol == formal->symbol &&
             (projected->projections.size() > 1 ||
              projected->type == formal->type.arguments.front());
    if (std::find(parameter.carriers.begin(), parameter.carriers.end(),
                  projected->binding) == parameter.carriers.end())
      return false;
    const MirInstruction *initialize = nullptr;
    const MirPlace *destination = nullptr;
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *target = instruction.destination
                                 ? body.findPlace(*instruction.destination)
                                 : nullptr;
        if (target == nullptr || target->root != MirPlaceRootKind::Binding ||
            target->binding != projected->binding ||
            !target->projections.empty())
          continue;
        if (initialize != nullptr ||
            instruction.kind != MirInstructionKind::Initialize)
          return false;
        initialize = &instruction;
        destination = target;
      }
    if (initialize == nullptr ||
        destination->type.kind != SemanticType::Reference ||
        destination->type.arguments.size() != 1 ||
        initialize->operands.size() != 1)
      return false;
    const auto &input = initialize->operands.front();
    const auto *address = body.findPlace(input.place);
    if (input.kind != MirOperandKind::Loan || input.loan != parameter.id ||
        input.value != 0 || address == nullptr ||
        input.type != destination->type.arguments.front() ||
        address->type != input.type)
      return false;
    if (address->root == MirPlaceRootKind::Loan) {
      const auto *result = body.findLoan(address->loan);
      return result != nullptr && result->kind == MirLoanKind::CallResult &&
             referenceAddressRetainsLoan(body, *address, parameter) &&
             self(self, result->source, depth + 1);
    }
    return self(self, address->id, depth + 1);
  };
  return contains(contains, place, 0);
}

bool referenceAddressRetainsLoan(const MirBody &body, const MirPlace &address,
                                 const MirLoan &retained) {
  if (address.root != MirPlaceRootKind::Loan || address.loan == retained.id)
    return true;
  std::unordered_set<MirLoanId> visited;
  MirLoanId link = address.loan;
  while (link != retained.id) {
    const MirLoan *child = body.findLoan(link);
    if (child == nullptr || !visited.insert(link).second)
      return false;
    if (child->kind == MirLoanKind::CallResult) {
      const MirInstruction *producer = nullptr;
      for (const MirBlock &block : body.blocks)
        for (const MirInstruction &instruction : block.instructions)
          if (instruction.kind == MirInstructionKind::Call &&
              instruction.loan == child->id &&
              instruction.hirValue == child->producedBy) {
            if (producer != nullptr)
              return false;
            producer = &instruction;
          }
      if (producer == nullptr || child->producedBy == 0 ||
          producer->info.access != child->access)
        return false;
      const auto &result = producer->info.type;
      const SemanticType &referent =
          result.kind == SemanticType::Reference && result.arguments.size() == 1
              ? result.arguments.front()
              : result;
      if (link == address.loan && referent != address.type)
        return false;
      // The call that produced this address may also produce the binding's
      // durable loan as its sibling: both reborrow one parent with the same
      // access, as a structural range element does beside its dereference.
      if (link == address.loan && child->parent != 0 &&
          child->parent == retained.parent &&
          child->producedBy == retained.producedBy &&
          child->access == retained.access &&
          retained.kind != MirLoanKind::CallResult)
        return true;
      // A call on a checked field (or on a captured reference carrier) names
      // that precise receiver storage as its dependency. Do not let the loan
      // table replace it with a coarse formal or an unrelated same-typed place.
      if (retained.kind == MirLoanKind::Parameter &&
          child->parent == retained.id &&
          producer->borrowOrigin == BorrowOriginKind::Receiver &&
          producer->receiver) {
        const auto *formal = body.findPlace(retained.source);
        const auto *receiver = body.findPlace(producer->receiver->place);
        const auto *source = body.findPlace(child->source);
        if (formal != nullptr && receiver != nullptr &&
            receiver->root == MirPlaceRootKind::Binding &&
            ((receiver->binding == formal->binding &&
              receiver->projections.size() > 1) ||
             (receiver->binding != formal->binding &&
              child->source != retained.source)) &&
            (source == nullptr || source->root != receiver->root ||
             source->binding != receiver->binding ||
             source->symbol != receiver->symbol ||
             source->projections != receiver->projections ||
             source->type != receiver->type))
          return false;
      }
      // Shared carrier extraction can publish a fresh address loan with no
      // reborrow parent. Its exact external source and dependency identities
      // still agree with the retained read-only owner; a mutable parent cannot
      // use this parallel-loan family to hide a detached exclusive reborrow.
      if (child->parent == 0 && child->access == AccessMode::ReadOnly &&
          retained.access == AccessMode::ReadOnly &&
          child->source == retained.source &&
          child->storedDependency == retained.storedDependency &&
          child->additionalDependencies == retained.additionalDependencies)
        return true;
    }
    link = child->parent;
  }
  return !visited.empty();
}

bool generatedUniqueConstruction(const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::AllocateUniqueOwner ||
      instruction.constructorTarget || instruction.functionTarget ||
      instruction.receiver || !instruction.result ||
      instruction.info.type.kind != SemanticType::UniqueOwner ||
      instruction.info.type.arguments.size() != 1 ||
      instruction.info.type.arguments.front().kind != SemanticType::Class ||
      instruction.operands.size() != 1 ||
      instruction.parameterTypes.size() != 1)
    return false;
  const auto &pointee = instruction.info.type.arguments.front();
  const auto &parameter = instruction.parameterTypes.front();
  const auto &operand = instruction.operands.front();
  if (operand.kind != MirOperandKind::Value || operand.value == 0 ||
      operand.place != 0 || operand.loan != 0 || operand.literal ||
      operand.type != parameter)
    return false;
  return (instruction.constructorKind == ConstructorKind::Copy &&
          parameter.kind == SemanticType::Reference &&
          parameter.referenceAccess == AccessMode::ReadOnly &&
          parameter.arguments == std::vector<SemanticType>{pointee}) ||
         (instruction.constructorKind == ConstructorKind::Move &&
          parameter == pointee);
}

bool bindingReceivedStoredLoan(const MirBody &body, HirBindingId binding,
                               MirLoanId dependency, std::size_t depth) {
  const auto *loan = body.findLoan(dependency);
  if (depth > body.places.size() || loan == nullptr ||
      loan->kind != MirLoanKind::Stored || !loan->lifecycleBound)
    return false;
  const MirInstruction *publication = nullptr;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions) {
      const auto *place = instruction.destination
                              ? body.findPlace(*instruction.destination)
                              : nullptr;
      if (instruction.kind != MirInstructionKind::Initialize ||
          place == nullptr || place->root != MirPlaceRootKind::Binding ||
          place->binding != binding || !place->projections.empty())
        continue;
      if (publication != nullptr)
        return false;
      publication = &instruction;
    }
  if (publication == nullptr || publication->operands.size() != 1 ||
      publication->operands.front().kind != MirOperandKind::Value)
    return false;
  const auto *producer =
      definitionFor(body, publication->operands.front().value);
  const auto *destination = body.findPlace(*publication->destination);
  if (producer == nullptr || destination == nullptr ||
      producer->info.type != destination->type ||
      !destination->traits.containsBorrowedState)
    return false;
  if (producer->loan == dependency)
    return true;
  if (producer->kind != MirInstructionKind::Move ||
      producer->operands.size() != 1 ||
      producer->operands.front().kind != MirOperandKind::Move)
    return false;
  const auto *previous = body.findPlace(producer->operands.front().place);
  return previous != nullptr && previous->root == MirPlaceRootKind::Binding &&
         previous->projections.empty() && previous->type == destination->type &&
         bindingReceivedStoredLoan(body, previous->binding, dependency,
                                   depth + 1);
}

bool completeObjectType(const MirProgram &program,
                        const SemanticType &selected) {
  const auto eligible = [&](const auto &self, const SemanticType &type,
                            std::size_t depth) -> bool {
    if (depth > 128 || type.arrayLengthParameterId != 0)
      return false;
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
      return type == SemanticType(type.kind) ||
             (type.nativeCScalar != NativeCScalarKind::None &&
              type == SemanticType::nativeCScalarType(type.nativeCScalar,
                                                      program.target()));
    case SemanticType::RawPointer:
      return type.arguments.size() == 1 &&
             type.arguments.front().kind != SemanticType::TypeParameter &&
             type.arguments.front().kind != SemanticType::Unknown;
    case SemanticType::Enum:
      return program.findIntegralEnum(type) != nullptr;
    case SemanticType::Array:
      return type.arguments.size() == 1 && type.arrayLength > 0 &&
             self(self, type.arguments.front(), depth + 1);
    case SemanticType::Class: {
      const MirClassInstance *owner = nullptr;
      for (const MirClassInstance &candidate : program.classInstances()) {
        if (candidate.type == type) {
          if (owner != nullptr)
            return false;
          owner = &candidate;
        }
      }
      return owner != nullptr && owner->kind != ClassKind::Interface &&
             !owner->abstract && !owner->cOpaqueHandle &&
             owner->compilerCapability == CompilerCapabilityTypeKind::None;
    }
    default:
      return false;
    }
  };
  return eligible(eligible, selected, 0);
}

bool objectRelocate(const MirProgram &program,
                    const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::ObjectRelocate ||
      instruction.unsafeOperation != UnsafeOperationKind::ObjectLifetime ||
      instruction.info.type != SemanticType::Void ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.info.traits != semanticTraits(SemanticType::Void) ||
      instruction.info.updateArithmeticType != SemanticType::Unknown ||
      instruction.operands.size() != 2 ||
      instruction.parameterTypes.size() != 2)
    return false;
  const auto &pointer = instruction.parameterTypes.front();
  if (pointer.kind != SemanticType::RawPointer ||
      pointer.pointerAccess != AccessMode::Mutable ||
      pointer.arguments.size() != 1 ||
      !completeObjectType(program, pointer.arguments.front()) ||
      !mirTypeRelocationIsDefinedFailureFree(program,
                                             pointer.arguments.front()) ||
      instruction.parameterTypes[1] != pointer ||
      !std::all_of(instruction.operands.begin(), instruction.operands.end(),
                   [&](const MirOperand &operand) {
                     return operand.kind == MirOperandKind::Value &&
                            operand.type == pointer;
                   }))
    return false;
  const MirInstruction exact{.id = instruction.id,
                             .kind = MirInstructionKind::Call,
                             .hirValue = instruction.hirValue,
                             .hirStatement = instruction.hirStatement,
                             .callSite = instruction.callSite,
                             .unsafeOperation =
                                 UnsafeOperationKind::ObjectLifetime,
                             .operands = instruction.operands,
                             .parameterTypes = instruction.parameterTypes,
                             .intrinsic = IntrinsicKind::ObjectRelocate,
                             .info = instruction.info};
  return instruction == exact;
}

bool objectDestroy(const MirProgram &program,
                   const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::ObjectDestroy ||
      instruction.unsafeOperation != UnsafeOperationKind::ObjectLifetime ||
      instruction.result || instruction.info.type != SemanticType::Void ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.info.traits != semanticTraits(SemanticType::Void) ||
      instruction.info.updateArithmeticType != SemanticType::Unknown ||
      instruction.operands.size() != 1 ||
      instruction.parameterTypes.size() != 1)
    return false;
  const auto &pointer = instruction.parameterTypes.front();
  if (pointer.kind != SemanticType::RawPointer ||
      pointer.pointerAccess != AccessMode::Mutable ||
      pointer.arguments.size() != 1 ||
      !completeObjectType(program, pointer.arguments.front()) ||
      instruction.operands.front().kind != MirOperandKind::Value ||
      instruction.operands.front().type != pointer ||
      !instruction.definedFailure.localOrigins.empty() ||
      (instruction.definedFailure.propagation != FailurePropagationKind::None &&
       instruction.definedFailure.propagation !=
           FailurePropagationKind::Destructor))
    return false;
  const MirInstruction exact{.id = instruction.id,
                             .kind = MirInstructionKind::Call,
                             .hirValue = instruction.hirValue,
                             .hirStatement = instruction.hirStatement,
                             .callSite = instruction.callSite,
                             .unsafeOperation =
                                 UnsafeOperationKind::ObjectLifetime,
                             .operands = instruction.operands,
                             .parameterTypes = instruction.parameterTypes,
                             .intrinsic = IntrinsicKind::ObjectDestroy,
                             .definedFailure = instruction.definedFailure,
                             .elementCleanup = instruction.elementCleanup,
                             .info = instruction.info};
  return instruction == exact;
}

bool objectLayoutQuery(const MirProgram &program,
                       const MirInstruction &instruction) {
  if ((instruction.intrinsic != IntrinsicKind::ObjectSize &&
       instruction.intrinsic != IntrinsicKind::ObjectAlignment) ||
      !instruction.objectLayoutType ||
      instruction.kind != MirInstructionKind::Call || !instruction.result ||
      instruction.info.type != SemanticType::UInt64 ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.info.traits != semanticTraits(SemanticType::UInt64))
    return false;
  // All unlisted fields must remain neutral, including future ownership,
  // failure and invocation metadata. Provenance IDs do not grant authority.
  const MirInstruction exact{.id = instruction.id,
                             .kind = MirInstructionKind::Call,
                             .hirValue = instruction.hirValue,
                             .hirStatement = instruction.hirStatement,
                             .callSite = instruction.callSite,
                             .result = instruction.result,
                             .intrinsic = instruction.intrinsic,
                             .objectLayoutType = instruction.objectLayoutType,
                             .info = instruction.info};
  if (instruction != exact)
    return false;
  return completeObjectType(program, *instruction.objectLayoutType);
}

bool objectConstruct(const MirProgram &program,
                     const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::ObjectConstruct ||
      instruction.unsafeOperation != UnsafeOperationKind::ObjectLifetime ||
      instruction.result || instruction.info.type != SemanticType::Void ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.info.traits != semanticTraits(SemanticType::Void) ||
      instruction.info.updateArithmeticType != SemanticType::Unknown ||
      instruction.parameterTypes.empty() ||
      instruction.parameterTypes.size() != instruction.operands.size())
    return false;
  const auto &pointer = instruction.parameterTypes.front();
  if (pointer.kind != SemanticType::RawPointer ||
      pointer.pointerAccess != AccessMode::Mutable ||
      pointer.arguments.size() != 1 ||
      !completeObjectType(program, pointer.arguments.front()) ||
      !instruction.definedFailure.localOrigins.empty() ||
      (instruction.definedFailure.propagation != FailurePropagationKind::None &&
       instruction.definedFailure.propagation !=
           FailurePropagationKind::Constructor))
    return false;
  // Call-input/lifecycle verification proves every prepared parameter and its
  // handoff; the program verifier binds constructor identity/signature/failure.
  for (std::size_t index = 0; index < instruction.operands.size(); ++index)
    if (instruction.operands[index].kind != MirOperandKind::Value ||
        instruction.operands[index].type != instruction.parameterTypes[index])
      return false;
  const auto &element = pointer.arguments.front();
  if (element.kind == SemanticType::Class) {
    for (const auto &owner : program.classInstances()) {
      if (owner.type != element)
        continue;
      if (owner.traits.containsBorrowedState)
        return false;
      if (!instruction.constructorTarget) {
        const bool transfer =
            instruction.parameterTypes.size() == 2 &&
            instruction.parameterTypes[1] == element &&
            ((instruction.constructorKind == ConstructorKind::Copy &&
              owner.traits.copyable) ||
             (instruction.constructorKind == ConstructorKind::Move &&
              owner.traits.movable));
        const bool defaultConstruction =
            instruction.parameterTypes.size() == 1 &&
            instruction.constructorKind == ConstructorKind::Ordinary &&
            owner.defaultConstructor == SpecialMemberStatus::Generated &&
            !owner.generatedDefaultRequiresSchedule;
        if (!transfer && !defaultConstruction)
          return false;
      }
    }
  } else if (instruction.constructorTarget || !instruction.lifecycle.empty() ||
             instruction.constructorKind != ConstructorKind::Ordinary ||
             !instruction.definedFailure.empty() ||
             instruction.parameterTypes.size() > 2 ||
             (instruction.parameterTypes.size() == 2 &&
              instruction.parameterTypes[1] != element)) {
    return false;
  }
  const MirInstruction exact{.id = instruction.id,
                             .kind = MirInstructionKind::Call,
                             .hirValue = instruction.hirValue,
                             .hirStatement = instruction.hirStatement,
                             .callSite = instruction.callSite,
                             .unsafeOperation =
                                 UnsafeOperationKind::ObjectLifetime,
                             .operands = instruction.operands,
                             .parameterTypes = instruction.parameterTypes,
                             .intrinsic = IntrinsicKind::ObjectConstruct,
                             .definedFailure = instruction.definedFailure,
                             .constructorTarget = instruction.constructorTarget,
                             .constructorKind = instruction.constructorKind,
                             .info = instruction.info,
                             .lifecycle = instruction.lifecycle};
  return instruction == exact;
}

bool rawPointerConversion(const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Compute ||
      instruction.operation != MirOperation::Convert ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.operands.size() != 1 || !instruction.result ||
      instruction.operands.front().kind != MirOperandKind::Value ||
      instruction.intrinsic != IntrinsicKind::None || instruction.loan ||
      instruction.rawMemoryAccess ||
      instruction.unsafeOperation != UnsafeOperationKind::None ||
      instruction.borrowOrigin != BorrowOriginKind::None ||
      !instruction.lifecycle.empty() || instruction.ownership ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty())
    return false;
  const auto &source = instruction.operands.front().type;
  const auto &target = instruction.info.type;
  return source.kind == SemanticType::RawPointer &&
         target.kind == SemanticType::RawPointer &&
         source.arguments.size() == 1 && target.arguments.size() == 1 &&
         (source.pointerAccess == AccessMode::Mutable ||
          target.pointerAccess == AccessMode::ReadOnly) &&
         (source.arguments.front() == target.arguments.front() ||
          source.arguments.front() == SemanticType::Void ||
          target.arguments.front() == SemanticType::Void);
}

namespace {

[[nodiscard]] std::optional<std::uint64_t>
signedMinimumMagnitude(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Int8:
    return std::uint64_t{1} << 7U;
  case SemanticType::Int16:
    return std::uint64_t{1} << 15U;
  case SemanticType::Int32:
    return std::uint64_t{1} << 31U;
  case SemanticType::Int64:
    return std::uint64_t{1} << 63U;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] bool integerLiteralCarrierCanRepresent(const SemanticType &type,
                                                     std::uint64_t magnitude) {
  switch (type.kind) {
  case SemanticType::Int8:
    return magnitude <= static_cast<std::uint64_t>(INT8_MAX);
  case SemanticType::Int16:
    return magnitude <= static_cast<std::uint64_t>(INT16_MAX);
  case SemanticType::Int32:
    return magnitude <= static_cast<std::uint64_t>(INT32_MAX);
  case SemanticType::Int64:
    return magnitude <= static_cast<std::uint64_t>(INT64_MAX);
  case SemanticType::UInt8:
    return magnitude <= UINT8_MAX;
  case SemanticType::UInt16:
    return magnitude <= UINT16_MAX;
  case SemanticType::UInt32:
    return magnitude <= UINT32_MAX;
  case SemanticType::UInt64:
    return true;
  default:
    return false;
  }
}

} // namespace

// GTI parses the signed minimum as a positive magnitude followed by unary
// negation. The magnitude is intentionally one past the positive range, so it
// has no independently spellable value of the result type. Fuse the exact
// one-use source chain. Parentheses can retain an explicit MIR Identity
// between the magnitude and negation; those identities must be exact,
// failure-free, same-type, and single-use. Program-constant substitution may
// retain the result type on the magnitude, while a contextually typed direct
// literal may use a wider integer carrier that can represent the positive
// magnitude.
[[nodiscard]] const MirInstruction *
fusedSignedMinimumLiteral(const MirBody &body, const MirInstruction &negation) {
  if (negation.kind != MirInstructionKind::Compute ||
      negation.operation != MirOperation::Negate || !negation.result ||
      negation.operands.size() != 1 ||
      negation.operands.front().kind != MirOperandKind::Value ||
      !negation.localFailureSites.empty() || !negation.definedFailure.empty()) {
    return nullptr;
  }
  const MirInstruction *consumer = &negation;
  const MirValue *operand = body.findValue(negation.operands.front().value);
  const MirInstruction *literal = nullptr;
  while (operand != nullptr) {
    const MirInstruction *producer = definitionFor(body, operand->id);
    if (producer == nullptr || !producer->result ||
        *producer->result != operand->id) {
      return nullptr;
    }
    const std::vector<MirValueUse> &uses = body.usesOf(operand->id);
    if (uses.size() != 1 ||
        uses.front().kind != MirValueUseKind::InstructionOperand ||
        uses.front().instruction != consumer->id ||
        uses.front().operandIndex != 0) {
      return nullptr;
    }
    if (producer->operation != MirOperation::Identity) {
      literal = producer;
      break;
    }
    if (producer->kind != MirInstructionKind::Compute ||
        producer->operands.size() != 1 ||
        producer->operands.front().kind != MirOperandKind::Value ||
        producer->info.type != producer->operands.front().type ||
        !producer->localFailureSites.empty() ||
        !producer->definedFailure.empty()) {
      return nullptr;
    }
    consumer = producer;
    operand = body.findValue(producer->operands.front().value);
  }
  const std::optional<std::uint64_t> minimum =
      signedMinimumMagnitude(negation.info.type);
  const auto *magnitude = literal != nullptr && literal->literal
                              ? std::get_if<std::uint64_t>(&*literal->literal)
                              : nullptr;
  if (literal == nullptr || literal->kind != MirInstructionKind::Compute ||
      literal->operation != MirOperation::Literal || !literal->result ||
      operand == nullptr || *literal->result != operand->id ||
      literal->info.type != negation.operands.front().type ||
      !literal->localFailureSites.empty() || !literal->definedFailure.empty() ||
      !minimum || magnitude == nullptr || *magnitude != *minimum ||
      (literal->info.type != negation.info.type &&
       !integerLiteralCarrierCanRepresent(literal->info.type, *magnitude))) {
    return nullptr;
  }
  return literal;
}

[[nodiscard]] const MirInstruction *
fusedSignedMinimumNegation(const MirBody &body, const MirInstruction &carrier) {
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &candidate : block.instructions) {
      const MirInstruction *literal =
          fusedSignedMinimumLiteral(body, candidate);
      if (literal == nullptr) {
        continue;
      }
      const MirValue *operand =
          body.findValue(candidate.operands.front().value);
      while (operand != nullptr) {
        const MirInstruction *producer = definitionFor(body, operand->id);
        if (producer == nullptr) {
          break;
        }
        if (producer == &carrier) {
          return &candidate;
        }
        if (producer == literal ||
            producer->operation != MirOperation::Identity ||
            producer->operands.size() != 1 ||
            producer->operands.front().kind != MirOperandKind::Value) {
          break;
        }
        operand = body.findValue(producer->operands.front().value);
      }
    }
  }
  return nullptr;
}

const MirInstruction *definitionFor(const MirBody &body, MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirBlock *block =
      value == nullptr ? nullptr : body.findBlock(value->definitionBlock);
  if (block == nullptr) {
    return nullptr;
  }
  const auto found =
      std::find_if(block->instructions.begin(), block->instructions.end(),
                   [&](const MirInstruction &instruction) {
                     return instruction.id == value->definition;
                   });
  return found == block->instructions.end() ? nullptr : &*found;
}

std::optional<MirPlaceId> borrowSourceForPlace(const MirBody &body,
                                               MirPlaceId placeId,
                                               std::size_t depth) {
  if (depth > body.places.size() + body.values.size() + body.loans.size()) {
    return std::nullopt;
  }
  const MirPlace *place = body.findPlace(placeId);
  if (place == nullptr) {
    return std::nullopt;
  }
  if (place->root == MirPlaceRootKind::Loan) {
    const MirLoan *loan = body.findLoan(place->loan);
    return loan == nullptr ? std::nullopt
                           : std::optional<MirPlaceId>{loan->source};
  }
  if (place->root == MirPlaceRootKind::Value) {
    const std::optional<MirPlaceId> source =
        borrowSourceForValue(body, place->value, depth + 1);
    return source ? source : std::optional<MirPlaceId>{placeId};
  }
  if (place->root == MirPlaceRootKind::Binding) {
    const MirLoan *carrier = nullptr;
    for (const MirLoan &loan : body.loans) {
      if (std::find(loan.carriers.begin(), loan.carriers.end(),
                    place->binding) == loan.carriers.end()) {
        continue;
      }
      if (carrier != nullptr && carrier->source != loan.source) {
        return std::nullopt;
      }
      carrier = &loan;
    }
    if (carrier != nullptr) {
      return carrier->source;
    }
  }
  return placeId;
}

std::optional<MirPlaceId> borrowSourceForOperand(const MirBody &body,
                                                 const MirOperand &operand,
                                                 std::size_t depth) {
  switch (operand.kind) {
  case MirOperandKind::Loan: {
    const MirLoan *loan = body.findLoan(operand.loan);
    return loan == nullptr ? std::nullopt
                           : std::optional<MirPlaceId>{loan->source};
  }
  case MirOperandKind::Address:
  case MirOperandKind::Copy:
  case MirOperandKind::Move:
  case MirOperandKind::BorrowRead:
  case MirOperandKind::BorrowWrite:
    return borrowSourceForPlace(body, operand.place, depth + 1);
  case MirOperandKind::Value:
    return borrowSourceForValue(body, operand.value, depth + 1);
  case MirOperandKind::Constant:
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<MirPlaceId> borrowSourceForValue(const MirBody &body,
                                               MirValueId valueId,
                                               std::size_t depth) {
  if (depth > body.places.size() + body.values.size() + body.loans.size()) {
    return std::nullopt;
  }
  const MirInstruction *definition = definitionFor(body, valueId);
  if (definition == nullptr) {
    return std::nullopt;
  }
  if (definition->loan) {
    const MirLoan *loan = body.findLoan(*definition->loan);
    if (loan != nullptr) {
      return loan->source;
    }
  }
  if (definition->operands.size() == 1) {
    return borrowSourceForOperand(body, definition->operands.front(),
                                  depth + 1);
  }
  return std::nullopt;
}

const MirLoan *rawBorrowResultLoan(const MirBody &body,
                                   const MirInstruction &instruction) {
  const bool mutableBorrow =
      instruction.intrinsic == IntrinsicKind::RawBorrowMut;
  if ((!mutableBorrow && instruction.intrinsic != IntrinsicKind::RawBorrow) ||
      instruction.kind != MirInstructionKind::Call || !instruction.result ||
      instruction.functionTarget || instruction.receiver ||
      instruction.destination || instruction.operands.size() != 2 ||
      instruction.parameterTypes.size() != 2 ||
      instruction.unsafeOperation != UnsafeOperationKind::RawDereference ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty() ||
      !instruction.lifecycle.empty() || !instruction.loan ||
      instruction.borrowOrigin != BorrowOriginKind::Argument ||
      instruction.borrowArgument != 1 || !instruction.borrowArguments.empty() ||
      !instruction.borrowFields.empty() || instruction.borrowPlace)
    return nullptr;
  const auto access =
      mutableBorrow ? AccessMode::Mutable : AccessMode::ReadOnly;
  const auto &pointer = instruction.operands[0];
  const auto &witness = instruction.operands[1];
  const auto *address = pointer.kind == MirOperandKind::Value
                            ? body.findValue(pointer.value)
                            : nullptr;
  const auto source = borrowSourceForOperand(body, witness);
  const auto *loan = body.findLoan(*instruction.loan);
  const auto *parent = witness.kind == MirOperandKind::Loan
                           ? body.findLoan(witness.loan)
                           : nullptr;
  if (!address || address->info.type != pointer.type ||
      pointer.type.kind != SemanticType::RawPointer ||
      pointer.type.arguments.size() != 1 ||
      pointer.type.arguments.front() == SemanticType::Void ||
      (mutableBorrow && pointer.type.pointerAccess != AccessMode::Mutable) ||
      instruction.parameterTypes[0] != pointer.type ||
      instruction.parameterTypes[1].kind != SemanticType::Reference ||
      instruction.parameterTypes[1].arguments.size() != 1 ||
      instruction.parameterTypes[1].referenceAccess != access ||
      witness.type != instruction.parameterTypes[1] ||
      instruction.info.type != pointer.type.arguments.front() ||
      instruction.info.category != ValueCategory::Place ||
      instruction.info.access != access || instruction.borrowAccess != access ||
      !source || !loan || loan->kind != MirLoanKind::CallResult ||
      loan->source != *source || loan->access != access ||
      loan->producedBy != instruction.hirValue || loan->entry ||
      loan->escapes || loan->lifecycleBound ||
      (witness.kind == MirOperandKind::Loan &&
       (!parent || loan->parent != witness.loan ||
        (mutableBorrow && parent->access != AccessMode::Mutable))))
    return nullptr;
  return loan;
}

} // namespace lang::mir_detail
