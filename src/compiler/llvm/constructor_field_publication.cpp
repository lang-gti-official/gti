#include "constructor_field_publication.h"

#include "../mir_verification_queries.h"

#include <algorithm>

namespace lang::native_llvm {
namespace {

bool recordOnlyResult(const MirBody &body, MirValueId value) {
  for (const MirValueUse &use : body.usesOf(value)) {
    if (use.kind != MirValueUseKind::PlaceRoot)
      return false;
    const MirPlace *place = body.findPlace(use.place);
    if (!place || !place->projections.empty())
      return false;
    if (std::any_of(
            body.loans.begin(), body.loans.end(),
            [&](const MirLoan &loan) { return loan.source == place->id; }))
      return false;
    for (const MirBlock &block : body.blocks) {
      if (block.terminator.value && block.terminator.value->place == place->id)
        return false;
      for (const MirInstruction &candidate : block.instructions)
        if (candidate.destination == place->id ||
            (candidate.receiver && candidate.receiver->place == place->id) ||
            std::any_of(candidate.operands.begin(), candidate.operands.end(),
                        [&](const MirOperand &operand) {
                          return operand.place == place->id;
                        }))
          return false;
    }
  }
  return true;
}

UnstagedConstructorFieldPublicationTransport
publicationTransport(const MirProgram &program,
                     const MirInstruction &instruction,
                     const SemanticType &targetType) {
  if (instruction.info.type == targetType)
    return UnstagedConstructorFieldPublicationTransport::ExactValue;

  MirInstruction exactSourceNull;
  exactSourceNull.id = instruction.id;
  exactSourceNull.kind = MirInstructionKind::Compute;
  exactSourceNull.hirValue = instruction.hirValue;
  exactSourceNull.result = instruction.result;
  exactSourceNull.operation = MirOperation::Literal;
  exactSourceNull.literal = instruction.literal;
  exactSourceNull.literalProvenance.kind = MirLiteralProvenanceKind::Source;
  exactSourceNull.info = instruction.info;
  const auto pointerTarget =
      (targetType.kind == SemanticType::UniqueOwner &&
       targetType.arguments.size() == 1 &&
       mir_detail::completeObjectType(program,
                                      targetType.arguments.front())) ||
      targetType.kind == SemanticType::CString ||
      (targetType.kind == SemanticType::RawPointer &&
       targetType.arguments.size() == 1);
  const auto exactNull =
      pointerTarget && instruction.info.type == SemanticType::NullPtr &&
      instruction.info.category == ValueCategory::Value &&
      instruction.literal &&
      std::holds_alternative<std::nullptr_t>(*instruction.literal) &&
      instruction == exactSourceNull;
  return exactNull ? UnstagedConstructorFieldPublicationTransport::NullPointer
                   : UnstagedConstructorFieldPublicationTransport::None;
}

} // namespace

UnstagedConstructorFieldPublicationPlan unstagedConstructorFieldPublication(
    const MirProgram &program, const MirBody &body,
    HirConstructorInstanceId constructorId, const MirInstruction &instruction) {
  UnstagedConstructorFieldPublicationPlan result;
  result.initializer = mirRecordPublishedConstructorInitializer(
      program, body, constructorId, instruction);
  if (!result.initializer)
    return result;

  const auto *constructor = program.findConstructorInstance(constructorId);
  const auto *owner =
      constructor ? program.findClassInstance(constructor->owner) : nullptr;
  const auto *value =
      instruction.result ? body.findValue(*instruction.result) : nullptr;
  if (!constructor || &constructor->body != &body || !owner ||
      constructor->owner != owner->id || !instruction.result || !value ||
      value->definition != instruction.id ||
      value->info.type != instruction.info.type ||
      !recordOnlyResult(body, *instruction.result))
    return result;

  const auto &initializer = *result.initializer;
  const auto field =
      std::find_if(owner->declaredFields.begin(), owner->declaredFields.end(),
                   [&](const MirClassFieldInfo &candidate) {
                     return candidate.symbol == initializer.field &&
                            candidate.type == initializer.targetType;
                   });
  if (initializer.kind != ConstructorInitializerTargetKind::Field ||
      initializer.field == 0 || initializer.storesReference ||
      initializer.arguments != std::vector<HirValueId>{instruction.hirValue} ||
      field == owner->declaredFields.end() ||
      std::count_if(owner->declaredFields.begin(), owner->declaredFields.end(),
                    [&](const MirClassFieldInfo &candidate) {
                      return candidate.symbol == initializer.field;
                    }) != 1)
    return result;

  const auto transport =
      publicationTransport(program, instruction, initializer.targetType);
  if (transport == UnstagedConstructorFieldPublicationTransport::None)
    return result;

  std::size_t publishers = 0;
  for (const MirBlock &block : body.blocks)
    for (const MirInstruction &candidate : block.instructions) {
      if (candidate.tombstone || !candidate.result ||
          !recordOnlyResult(body, *candidate.result))
        continue;
      const auto *candidateValue = body.findValue(*candidate.result);
      if (candidateValue && candidateValue->definition == candidate.id &&
          candidateValue->info.type == candidate.info.type &&
          publicationTransport(program, candidate, initializer.targetType) !=
              UnstagedConstructorFieldPublicationTransport::None &&
          mirRecordPublishedConstructorInitializer(
              program, body, constructorId, candidate) == result.initializer)
        ++publishers;
    }
  if (publishers != 1)
    return result;

  result.value = *instruction.result;
  result.sourceType = instruction.info.type;
  result.targetType = initializer.targetType;
  result.transport = transport;
  result.exact = true;
  return result;
}

StoredFieldPublicationPlan
storedFieldPublication(const MirProgram &program, const MirBody &body,
                       HirConstructorInstanceId constructorId,
                       const MirLoan &loan) {
  StoredFieldPublicationPlan result;
  result.requested = loan.kind == MirLoanKind::Stored && loan.storedField != 0;
  if (!result.requested)
    return result;

  const auto *constructor = program.findConstructorInstance(constructorId);
  const auto *owner =
      constructor ? program.findClassInstance(constructor->owner) : nullptr;
  if (!constructor || &constructor->body != &body || !owner ||
      constructor->owner != owner->id)
    return result;

  const auto field =
      std::find_if(owner->declaredFields.begin(), owner->declaredFields.end(),
                   [&](const MirClassFieldInfo &candidate) {
                     return candidate.symbol == loan.storedField;
                   });
  const auto initializer = std::find_if(
      constructor->initializers.begin(), constructor->initializers.end(),
      [&](const MirConstructorInitializer &candidate) {
        return candidate.kind == ConstructorInitializerTargetKind::Field &&
               candidate.field == loan.storedField;
      });
  if (field == owner->declaredFields.end() ||
      initializer == constructor->initializers.end() ||
      std::count_if(owner->declaredFields.begin(), owner->declaredFields.end(),
                    [&](const MirClassFieldInfo &candidate) {
                      return candidate.symbol == loan.storedField;
                    }) != 1 ||
      std::count_if(
          constructor->initializers.begin(), constructor->initializers.end(),
          [&](const MirConstructorInitializer &candidate) {
            return candidate.kind == ConstructorInitializerTargetKind::Field &&
                   candidate.field == loan.storedField;
          }) != 1 ||
      field->type != initializer->targetType)
    return result;

  const bool reference = field->type.kind == SemanticType::Reference;
  if (initializer->storesReference != reference ||
      (reference && field->type.arguments.size() != 1))
    return result;

  result.initializer = &*initializer;
  result.field = &*field;
  result.kind = reference ? StoredFieldPublicationKind::ReferenceAddress
                          : StoredFieldPublicationKind::ProvenanceOnly;
  result.exact = true;
  return result;
}

} // namespace lang::native_llvm
