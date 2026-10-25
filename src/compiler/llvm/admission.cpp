#include "gti/llvm_admission.h"

#include "coverage.h"
#include "reachability.h"
#include "text.h"

#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace lang {
namespace {

using llvm_detail::coverage;
using llvm_detail::Shape;

// Representation eligibility, not source type checking: later storage families
// cannot enter the passive-array implementation through an enum-wide claim.
bool passiveArrayElement(const SemanticType &type) {
  if (type.kind == SemanticType::Array)
    return type.arguments.size() == 1 && !type.arrayLengthParameterId &&
           passiveArrayElement(type.arguments.front());
  return isPassiveScalarValueType(type) && type.kind != SemanticType::Void;
}

bool passiveAddress(const SemanticType &type) {
  if (type.kind != SemanticType::RawPointer || type.arguments.size() != 1)
    return false;
  const auto &pointee = type.arguments.front();
  return pointee.kind == SemanticType::Void || passiveArrayElement(pointee) ||
         passiveAddress(pointee);
}

bool passiveReferenceType(const SemanticType &type) {
  return passiveArrayElement(type) || passiveAddress(type) ||
         type.kind == SemanticType::StringView ||
         type.kind == SemanticType::CString ||
         (type.kind == SemanticType::Reference && type.arguments.size() == 1 &&
          (passiveArrayElement(type.arguments.front()) ||
           passiveAddress(type.arguments.front()) ||
           type.arguments.front().kind == SemanticType::StringView));
}

bool passiveTextViewAddress(const MirInstruction &stage) {
  if (stage.intrinsic != IntrinsicKind::TextViewFromAddress ||
      stage.kind != MirInstructionKind::Call || !stage.loan ||
      stage.borrowOrigin != BorrowOriginKind::Argument ||
      stage.borrowArgument != 2 || stage.operands.size() != 3 ||
      stage.parameterTypes.size() != 3 ||
      stage.info.type != SemanticType::StringView ||
      stage.parameterTypes[2].kind != SemanticType::Reference ||
      !passiveReferenceType(stage.operands[2].type))
    return false;
  const auto &pointer = stage.operands[0].type;
  return pointer.kind == SemanticType::RawPointer &&
         pointer.arguments.size() == 1 &&
         pointer.arguments[0] == SemanticType::Char &&
         stage.operands[1].type == SemanticType::UInt64;
}

bool passiveCStringAddress(const MirInstruction &stage) {
  if (stage.intrinsic != IntrinsicKind::CStringFromAddress ||
      stage.functionTarget || stage.kind != MirInstructionKind::Call ||
      !stage.loan || stage.borrowOrigin != BorrowOriginKind::Argument ||
      stage.borrowArgument != 1 || stage.operands.size() != 2 ||
      stage.parameterTypes.size() != 2 ||
      stage.info.type != SemanticType::CString ||
      stage.parameterTypes[1].kind != SemanticType::Reference ||
      !passiveReferenceType(stage.operands[1].type))
    return false;
  const auto &pointer = stage.operands[0].type;
  return pointer.kind == SemanticType::RawPointer &&
         pointer.arguments.size() == 1 &&
         pointer.arguments[0] == SemanticType::Char;
}

bool primitiveRawIndex(const MirInstruction &stage) {
  return stage.operation == MirOperation::Index && stage.operands.size() == 2 &&
         stage.operands[0].type.kind == SemanticType::RawPointer &&
         passiveAddress(stage.operands[0].type) &&
         stage.operands[0].type.arguments.front().kind != SemanticType::Void &&
         stage.info.type == stage.operands[0].type.arguments.front() &&
         constantIntegerDomain(stage.operands[1].type).has_value();
}

// The verifier establishes the witness's lifetime/access. Representation
// transports the independently evaluated pointer, not that witness's address.
bool passiveRawReference(const MirInstruction &stage) {
  const bool assertion = stage.intrinsic == IntrinsicKind::RawBorrow ||
                         stage.intrinsic == IntrinsicKind::RawBorrowMut;
  if (!assertion || stage.kind != MirInstructionKind::Call ||
      stage.functionTarget || stage.operands.size() != 2 ||
      stage.parameterTypes.size() != 2 || !stage.loan ||
      stage.borrowOrigin != BorrowOriginKind::Argument ||
      stage.borrowArgument != 1 ||
      stage.info.category != ValueCategory::Place ||
      !passiveAddress(stage.operands[0].type) ||
      stage.parameterTypes[1].kind != SemanticType::Reference)
    return false;
  return stage.info.type == stage.operands[0].type.arguments.front() &&
         passiveReferenceType(stage.info.type) &&
         passiveReferenceType(stage.operands[1].type);
}

bool primitivePointerArithmetic(const MirInstruction &stage) {
  if (stage.operands.size() != 2)
    return false;
  const bool reversed = stage.operation == MirOperation::PointerAdd &&
                        stage.operands[1].type.kind == SemanticType::RawPointer;
  const auto &pointer = stage.operands[reversed ? 1 : 0].type;
  if (!passiveAddress(pointer) ||
      pointer.arguments.front().kind == SemanticType::Void)
    return false;
  return stage.operation == MirOperation::PointerDifference
             ? stage.operands[1].type == pointer
             : (stage.operation == MirOperation::PointerAdd ||
                stage.operation == MirOperation::PointerSubtract) &&
                   stage.info.type == pointer &&
                   constantIntegerDomain(stage.operands[reversed ? 0 : 1].type)
                       .has_value();
}

bool primitiveRawAccess(const MirBody &body, const MirPlace &place) {
  const auto *value = body.findValue(place.value);
  return place.root == MirPlaceRootKind::Value && value &&
         passiveAddress(value->info.type) &&
         value->info.type.arguments.front().kind != SemanticType::Void &&
         place.type == value->info.type.arguments.front() &&
         place.projections.size() == 1 &&
         (place.projections.front().kind == MirProjectionKind::RawIndex ||
          place.projections.front().kind == MirProjectionKind::RawDereference);
}

bool arrayProjection(const MirBody &body, const MirPlace &place) {
  const auto root = std::find_if(
      body.places.begin(), body.places.end(), [&](const auto &candidate) {
        return candidate.projections.empty() && candidate.root == place.root &&
               candidate.binding == place.binding &&
               candidate.symbol == place.symbol &&
               candidate.temporary == place.temporary &&
               candidate.value == place.value && candidate.loan == place.loan;
      });
  if (root == body.places.end())
    return false;
  auto type = root->type;
  const auto &array =
      type.kind == SemanticType::Reference && type.arguments.size() == 1
          ? type.arguments.front()
          : type;
  if (array.kind != SemanticType::Array || !passiveArrayElement(array))
    return false;
  for (const auto &projection : place.projections) {
    if (!((projection.kind == MirProjectionKind::Index &&
           type.kind == SemanticType::Array) ||
          (projection.kind == MirProjectionKind::Dereference &&
           type.kind == SemanticType::Reference)))
      return false;
    auto element = type.arguments.front();
    type = std::move(element);
  }
  return type == place.type;
}

// One representative witness and count per requirement/body/context. Repeated
// scalar operations do not allocate a duplicate diagnostic record per use.
class Inventory final {
public:
  Inventory(std::vector<LlvmCoverageUse> &uses, const MirProgram &program)
      : uses(uses), program(program) {}

  std::optional<MirBodyAddress> body;
  SourceSpan source;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  bool required = true;
  bool executable = false;
  const MirBody *currentBody = nullptr;

  bool nativeValue(const SemanticType &type) const {
    std::set<HirClassInstanceId> visiting;
    std::set<const MirPayloadEnum *> payloads;
    const auto supported = [&](const auto &self,
                               const SemanticType &selected) -> bool {
      if (passiveReferenceType(selected) || passiveAddress(selected) ||
          selected == SemanticType::Void ||
          selected == SemanticType::StringView ||
          selected == SemanticType::CString ||
          selected == SemanticType::NullPtr)
        return true;
      if (selected.kind == SemanticType::Reference ||
          selected.kind == SemanticType::RawPointer)
        return selected.arguments.size() == 1 &&
               selected.arguments[0].kind != SemanticType::Unknown &&
               selected.arguments[0].kind != SemanticType::TypeParameter;
      if (selected.kind == SemanticType::Enum) {
        if (const auto *payload = program.findPayloadEnum(selected)) {
          if (!payloads.insert(payload).second)
            return false;
          bool valid = true;
          for (const auto &variant : payload->variants)
            for (const auto &field : variant.payloadTypes)
              valid = valid && self(self, field);
          payloads.erase(payload);
          return valid;
        }
        return std::any_of(
            program.integralEnums().begin(), program.integralEnums().end(),
            [&](const auto &entry) { return entry.type == selected; });
      }
      if (selected.kind == SemanticType::Array)
        return selected.arguments.size() == 1 &&
               !selected.arrayLengthParameterId &&
               self(self, selected.arguments[0]);
      if (selected.kind == SemanticType::TypePack && selected.concretePack)
        return std::all_of(
            selected.arguments.begin(), selected.arguments.end(),
            [&](const auto &element) { return self(self, element); });
      if (selected.kind != SemanticType::Class)
        return false;
      const MirClassInstance *owner = nullptr;
      for (const auto &candidate : program.classInstances())
        if (candidate.type == selected) {
          if (owner)
            return false;
          owner = &candidate;
        }
      if (!owner ||
          owner->compilerCapability != CompilerCapabilityTypeKind::None ||
          owner->polymorphic || owner->abstract || owner->cOpaqueHandle ||
          !owner->bases.empty() || !owner->structuralBases.empty() ||
          !visiting.insert(owner->id).second)
        return false;
      bool valid = true;
      for (const auto &field : owner->declaredFields)
        valid = valid && self(self, field.type);
      visiting.erase(owner->id);
      return valid;
    };
    return supported(supported, type);
  }
  std::optional<Shape> classShape(const SemanticType &type) const {
    if (type.kind == SemanticType::TypePack && type.concretePack &&
        currentBody && nativeValue(type) &&
        std::none_of(currentBody->values.begin(), currentBody->values.end(),
                     [](const auto &value) {
                       return value.info.type.kind == SemanticType::TypePack;
                     }) &&
        std::any_of(currentBody->places.begin(), currentBody->places.end(),
                    [&](const auto &place) {
                      return place.root == MirPlaceRootKind::Binding &&
                             place.projections.empty() &&
                             place.initiallyAvailable && place.type == type;
                    }))
      return Shape::ConcreteParameterPack;
    const auto *leaf = &type;
    while ((leaf->kind == SemanticType::Array ||
            leaf->kind == SemanticType::Reference ||
            leaf->kind == SemanticType::RawPointer) &&
           leaf->arguments.size() == 1)
      leaf = &leaf->arguments[0];
    if (leaf->kind == SemanticType::Class && nativeValue(type))
      return std::any_of(program.classInstances().begin(),
                         program.classInstances().end(),
                         [&](const auto &owner) {
                           return owner.type == *leaf && owner.unionLayout;
                         })
                 ? Shape::NativeUnionStorage
                 : Shape::NativeClassStorage;
    if (leaf->kind == SemanticType::Enum && program.findPayloadEnum(*leaf) &&
        nativeValue(type))
      return Shape::NativePayloadStorage;
    return {};
  }
  const MirClassInstance *bodyClass() const {
    if (!body)
      return nullptr;
    if (body->kind == MirBodyKind::FieldInitializers ||
        body->kind == MirBodyKind::StaticFieldInitializers)
      return program.findClassInstance(body->owner);
    if (body->kind == MirBodyKind::Constructor)
      return program.findClassInstance(
          program.findConstructorInstance(body->owner)->owner);
    if (body->kind == MirBodyKind::Destructor)
      return program.findClassInstance(
          program.findDestructorInstance(body->owner)->owner);
    if (body->kind == MirBodyKind::Function) {
      const auto *target = program.findFunctionInstance(body->owner);
      if (target->owner)
        return program.findClassInstance(*target->owner);
    }
    return nullptr;
  }
  std::optional<Shape> bodyClassShape() const {
    if (body && body->kind == MirBodyKind::ThreadLocalInitializer)
      if (const auto *storage = program.findThreadLocalStorage(body->owner))
        if (nativeValue(storage->type))
          return Shape::NativeThreadStorage;
    const auto *owner = bodyClass();
    return owner ? classShape(owner->type) : std::nullopt;
  }
  std::optional<Shape> placeClassShape(const MirPlace &place) const {
    if (const auto selected = classShape(place.type))
      return selected;
    if (place.root == MirPlaceRootKind::This)
      return bodyClassShape();
    if (place.root == MirPlaceRootKind::Value && currentBody) {
      const auto *value = currentBody->findValue(place.value);
      if (value && value->info.category == ValueCategory::Place)
        for (const auto &block : currentBody->blocks)
          for (const auto &stage : block.instructions)
            if (!stage.tombstone && stage.result == place.value &&
                stage.operation == MirOperation::PayloadExtract)
              return classShape(mirInstructionEnumType(stage));
      return value ? classShape(value->info.type) : std::nullopt;
    }
    if (currentBody)
      for (const auto &root : currentBody->places)
        if (root.root == place.root && root.binding == place.binding &&
            root.symbol == place.symbol && root.temporary == place.temporary &&
            root.projections.empty())
          if (const auto selected = classShape(root.type))
            return selected;
    return {};
  }

  bool opaqueReferent(const SemanticType &type) const {
    if (type.kind != SemanticType::Class)
      return false;
    return std::any_of(program.classInstances().begin(),
                       program.classInstances().end(),
                       [&](const auto &instance) {
                         return instance.type == type &&
                                instance.compilerCapability ==
                                    CompilerCapabilityTypeKind::ErasedObject;
                       });
  }
  bool passiveReference(const SemanticType &type) const {
    return passiveReferenceType(type) || opaqueReferent(type) ||
           (type.kind == SemanticType::Reference &&
            type.arguments.size() == 1 && opaqueReferent(type.arguments[0]));
  }
  bool erasedReference(const MirInstruction &stage) const {
    if (stage.kind != MirInstructionKind::Call || !stage.loan ||
        stage.functionTarget || stage.operands.size() != 1 ||
        stage.parameterTypes.size() != 1 ||
        stage.parameterTypes[0].kind != SemanticType::Reference ||
        stage.info.category != ValueCategory::Place)
      return false;
    const bool erase = stage.intrinsic == IntrinsicKind::EraseReference ||
                       stage.intrinsic == IntrinsicKind::EraseReferenceMut;
    const bool restore = stage.intrinsic == IntrinsicKind::RestoreReference ||
                         stage.intrinsic == IntrinsicKind::RestoreReferenceMut;
    return (erase && opaqueReferent(stage.info.type) &&
            passiveReferenceType(stage.parameterTypes[0])) ||
           (restore && opaqueReferent(stage.parameterTypes[0].arguments[0]) &&
            passiveReferenceType(stage.info.type));
  }
  bool restoredLoanTransport(const MirBody &body,
                             const MirInstruction &stage) const {
    if (stage.kind != MirInstructionKind::Borrow || !stage.loan ||
        stage.operands.size() != 1 ||
        stage.operands[0].kind != MirOperandKind::Loan ||
        !passiveReference(stage.info.type))
      return false;
    const auto *target = body.findLoan(*stage.loan);
    const auto *source = body.findLoan(stage.operands[0].loan);
    if (!target || !source || target->access != stage.info.access ||
        source->access != target->access ||
        !(target->parent == source->id ||
          (target->kind == MirLoanKind::Return && target->escapes &&
           target->source == source->source &&
           target->producedBy == source->producedBy)))
      return false;
    for (const auto &block : body.blocks)
      for (const auto &producer : block.instructions)
        if (!producer.tombstone && producer.loan == stage.operands[0].loan &&
            producer.restoresReferenceResult && erasedReference(producer) &&
            producer.info.type == stage.info.type &&
            producer.info.access == stage.info.access)
          return true;
    return false;
  }

  template <class T> void add(T value, std::optional<Shape> implemented = {}) {
    const LlvmCoverageEntry &entry = coverage(value);
    const auto *shape = implemented ? &coverage(*implemented) : nullptr;
    const auto key =
        std::tuple(entry.family, entry.name, body.has_value(),
                   body ? body->kind : MirBodyKind::Module,
                   body ? body->owner : 0, required, executable, shape);
    auto [found, inserted] = indices.try_emplace(key, uses.size());
    if (inserted) {
      uses.push_back({&entry, body, source, block, instruction, 0, required,
                      executable, shape});
    }
    ++uses[found->second].occurrences;
  }

  void type(const SemanticType &root) {
    std::vector<const SemanticType *> pending{&root};
    while (!pending.empty()) {
      const SemanticType &type = *pending.back();
      pending.pop_back();
      if (program.findPayloadEnum(type))
        add(Shape::PayloadEnum, classShape(type));
      if (type.kind == SemanticType::Class)
        for (const auto &owner : program.classInstances())
          if (owner.type == type && owner.unionLayout)
            add(Shape::NativeUnion, classShape(type));
      const bool passiveArray =
          type.kind == SemanticType::Array && passiveArrayElement(type);
      const bool arrayAddress = type.kind == SemanticType::RawPointer &&
                                type.arguments.size() == 1 &&
                                passiveArrayElement(type.arguments.front());
      const bool reference =
          type.kind == SemanticType::Reference && passiveReference(type);
      const bool address = passiveAddress(type) ||
                           type.kind == SemanticType::NullPtr ||
                           type.kind == SemanticType::CString;
      add(type.kind,
          opaqueReferent(type) ? std::optional(Shape::ErasedReference)
          : address            ? std::optional(Shape::PassiveAddressValue)
          : passiveArray       ? std::optional(Shape::PassiveArrayValue)
          : reference          ? std::optional(Shape::PassiveReference)
          : arrayAddress       ? std::optional(Shape::CheckedArrayProjection)
                               : classShape(type));
      if (type.nativeCScalar != NativeCScalarKind::None) {
        add(type.nativeCScalar);
        add(Shape::NativeScalar);
      }
      if (type.kind == SemanticType::Array)
        add(Shape::FixedArray, passiveArray
                                   ? std::optional(Shape::PassiveArrayValue)
                                   : classShape(type));
      if (!type.arguments.empty())
        add(Shape::TypeArguments);
      for (const SemanticType &argument : type.arguments)
        pending.push_back(&argument);
      for (const SemanticType &argument : type.lambdaEnclosingClassTypes)
        pending.push_back(&argument);
      for (const SemanticType &argument : type.lambdaEnclosingFunctionTypes)
        pending.push_back(&argument);
    }
  }

  void traits(const SemanticTypeTraits &traits, const SemanticType &value) {
    const auto shape = classShape(value);
    if (traits.containsBorrowedState)
      add(Shape::BorrowedCarrier, shape);
    add(traits.ownership, shape);
    add(traits.drop, shape);
    if (traits.drop != DropKind::Trivial) {
      add(Shape::CleanupValue, shape);
    }
    if ((value.kind == SemanticType::Class && !opaqueReferent(value)) ||
        value.kind == SemanticType::Lambda) {
      add(traits.copyable  ? Shape::CopyableValue
          : traits.movable ? Shape::MoveOnlyValue
                           : Shape::ImmovableValue,
          shape);
    }
  }

  void operand(const MirOperand &operand) {
    const auto *place =
        currentBody ? currentBody->findPlace(operand.place) : nullptr;
    const bool selectedReference =
        passiveReference(operand.type) &&
        (operand.kind == MirOperandKind::Address ||
         operand.kind == MirOperandKind::BorrowRead ||
         operand.kind == MirOperandKind::BorrowWrite ||
         operand.kind == MirOperandKind::Loan);
    add(operand.kind, selectedReference ? std::optional(Shape::PassiveReference)
                      : operand.kind == MirOperandKind::Address && place &&
                              arrayProjection(*currentBody, *place)
                          ? std::optional(Shape::CheckedArrayProjection)
                          : classShape(operand.type));
    type(operand.type);
  }

  void cleanup(const MirDropType &root) {
    std::vector<const MirDropType *> pending{&root};
    while (!pending.empty()) {
      const auto &drop = *pending.back();
      pending.pop_back();
      type(drop.type);
      const auto shape = classShape(drop.type);
      if (drop.fixedArray)
        add(Shape::ArrayDrop, shape);
      if (drop.destructor)
        add(Shape::UserDestructor, shape);
      if (drop.polymorphicCleanup)
        add(Shape::PolymorphicClass);
      if (drop.payloadVariant)
        add(Shape::PayloadEnum, shape);
      if (!drop.aggregate.empty())
        add(Shape::FieldDrop, shape);
      if (drop.requiresActiveCleanup)
        add(Shape::CleanupValue, shape);
      for (const auto &child : drop.aggregate)
        pending.push_back(&child);
    }
  }

  void lifecycle(const MirLifecycleEvent &event) {
    add(event.kind);
    if (event.conditional) {
      const auto *drop = currentBody
                             ? currentBody->findDropObligation(
                                   event.source ? event.source : event.target)
                             : nullptr;
      add(Shape::ConditionalDrop,
          drop ? classShape(drop->dropType.type) : std::nullopt);
    }
  }

  void inspect(const LoweredBody &row, const MirBody &mir, bool needed) {
    currentBody = &mir;
    body = row.identity.address;
    source = row.identity.source;
    required = needed && row.role == LoweredBodyRole::SourceExecutable;
    executable = false;
    block = 0;
    instruction = 0;
    add(row.role);
    add(row.identity.definition);
    add(mir.kind, bodyClassShape());
    if (mir.kind == MirBodyKind::Constructor) {
      const auto *constructor = program.findConstructorInstance(body->owner);
      for (const auto &initializer : constructor->initializers)
        if (initializer.storesReference &&
            std::none_of(
                mir.loans.begin(), mir.loans.end(), [&](const auto &loan) {
                  return loan.kind == MirLoanKind::Stored &&
                         loan.storedField == initializer.field &&
                         (loan.entry ||
                          std::any_of(
                              mir.blocks.begin(), mir.blocks.end(),
                              [&](const auto &block) {
                                return std::any_of(
                                    block.instructions.begin(),
                                    block.instructions.end(),
                                    [&](const auto &instruction) {
                                      return !instruction.tombstone &&
                                             instruction.kind ==
                                                 MirInstructionKind::Borrow &&
                                             instruction.loan == loan.id &&
                                             instruction.operands.size() == 1;
                                    });
                              }));
                }))
          add(Shape::StoredFieldPublication);
    }
    if (row.role != LoweredBodyRole::SourceExecutable)
      return;
    executable = true;
    type(mir.returnType);
    add(mir.returnType.kind == SemanticType::Reference ? Shape::BorrowReturn
        : mir.returnType.kind == SemanticType::Class ||
                mir.returnType.kind == SemanticType::Lambda
            ? Shape::OwnedReturn
            : Shape::DirectReturn,
        mir.returnType.kind == SemanticType::Reference &&
                passiveReference(mir.returnType)
            ? std::optional(Shape::PassiveReference)
            : classShape(mir.returnType));
    if (mir.hasCallerLocation)
      add(Shape::CallerLocation);
    for (const auto &value : mir.values) {
      type(value.info.type);
      traits(value.info.traits, value.info.type);
    }
    for (const auto &place : mir.places) {
      add(place.root,
          primitiveRawAccess(mir, place)
              ? std::optional(Shape::PrimitiveRawAccess)
          : place.root == MirPlaceRootKind::Loan && passiveReference(place.type)
              ? std::optional(Shape::PassiveReference)
              : placeClassShape(place));
      add(place.access);
      type(place.type);
      traits(place.traits, place.type);
      for (const auto &projection : place.projections)
        add(projection.kind,
            primitiveRawAccess(mir, place)
                ? std::optional(Shape::PrimitiveRawAccess)
            : arrayProjection(mir, place)
                ? std::optional(Shape::CheckedArrayProjection)
            : projection.kind == MirProjectionKind::Dereference &&
                    passiveReference(place.type)
                ? std::optional(Shape::PassiveReference)
                : placeClassShape(place));
      if (place.foreignStorage)
        add(Shape::NativeCall);
    }
    for (const auto &loan : mir.loans) {
      const auto *origin = mir.findPlace(loan.source);
      const auto nativeConditions = [&](const auto &conditions) {
        return std::all_of(
            conditions.begin(), conditions.end(), [&](const auto &condition) {
              const auto *payload =
                  program.findPayloadEnum(condition.enumeration);
              return payload && condition.variant < payload->variants.size() &&
                     nativeValue(condition.enumeration);
            });
      };
      bool nativeBorrow = origin && nativeValue(origin->type) &&
                          nativeConditions(loan.conditions);
      for (const auto &dependency : loan.additionalDependencies) {
        const auto *source = mir.findPlace(dependency.source);
        nativeBorrow &= nativeConditions(dependency.conditions) &&
                        (source ? nativeValue(source->type)
                                : !dependency.source &&
                                      !dependency.designatedFields.empty());
      }
      if (loan.referenceSelection) {
        const auto *value = mir.findValue(loan.referenceSelection->value);
        nativeBorrow &= value && nativeValue(value->info.type) &&
                        nativeConditions(loan.referenceSelection->conditions);
      }
      const auto borrowShape = nativeBorrow
                                   ? std::optional(Shape::NativeBorrowMetadata)
                                   : std::nullopt;
      const bool selected = origin && passiveReference(origin->type) &&
                            !loan.referenceSelection && loan.conditions.empty();
      add(loan.kind, borrowShape ? borrowShape
                     : selected  ? std::optional(Shape::PassiveReference)
                     : origin    ? placeClassShape(*origin)
                                 : std::nullopt);
      add(loan.access);
      if (loan.referenceSelection)
        add(Shape::SelectedReferenceAddress, borrowShape);
      if (loan.twoPhase)
        add(Shape::TwoPhaseBorrow,
            origin ? placeClassShape(*origin) : std::nullopt);
      if (!loan.additionalDependencies.empty())
        add(Shape::MultipleBorrowOrigins, borrowShape);
      if (!loan.conditions.empty())
        add(Shape::ConditionalBorrow, borrowShape);
      for (const auto &dependency : loan.additionalDependencies) {
        add(dependency.access);
        if (!dependency.conditions.empty())
          add(Shape::ConditionalBorrow, borrowShape);
      }
    }
    for (const auto &drop : mir.dropObligations) {
      add(drop.kind, classShape(drop.dropType.type));
      cleanup(drop.dropType);
    }
    for (const auto &boundary : mir.cleanupBoundaries)
      add(boundary.kind);
    for (const auto &record : mir.failureRecords)
      add(record.kind);

    std::map<std::pair<MirBlockId, MirInstructionId>, const SourceSpan *> spans;
    for (const auto &location : row.sourceLocations)
      spans.emplace(std::pair(location.block, location.instruction),
                    &location.source);
    for (const auto &mirBlock : mir.blocks) {
      block = mirBlock.id;
      for (const auto &stage : mirBlock.instructions) {
        instruction = stage.id;
        const auto found = spans.find({block, instruction});
        source = found == spans.end() ? row.identity.source : *found->second;
        if (stage.tombstone) {
          add(Shape::Tombstone);
          continue;
        }
        std::optional<Shape> storageShape;
        if (stage.kind == MirInstructionKind::EnsureThreadLocal)
          if (const auto *storage =
                  program.findThreadLocalStorage(stage.threadLocalStorage))
            if (nativeValue(storage->type))
              storageShape = Shape::NativeThreadStorage;
        if (stage.kind == MirInstructionKind::Construct)
          storageShape = classShape(stage.info.type);
        if (stage.kind == MirInstructionKind::Drop ||
            stage.kind == MirInstructionKind::CleanupBeforeAssign)
          for (const auto &drop : mir.dropObligations)
            for (const auto &event : stage.lifecycle)
              if (event.source == drop.id)
                storageShape = classShape(drop.dropType.type);
        if (stage.kind == MirInstructionKind::CleanupBeforeAssign &&
            stage.replacementCleanup)
          storageShape = classShape(stage.replacementCleanup->type);
        add(stage.kind, stage.kind == MirInstructionKind::Borrow
                            ? (passiveReference(stage.info.type)
                                   ? std::optional(Shape::PassiveReference)
                                   : classShape(stage.info.type))
                            : storageShape);
        if (stage.callInputRole) {
          if (stage.info.type == SemanticType::CString &&
              stage.operands.size() == 1 &&
              stage.operands[0].type == SemanticType::StringView)
            add(Shape::CStringLiteralInput,
                native_llvm::wholeCStringLiteral(mir, stage.operands[0])
                    ? std::optional(Shape::PassiveAddressValue)
                    : std::nullopt);
          add(*stage.callInputRole, classShape(stage.info.type));
          add(stage.callInputKind,
              passiveReference(stage.info.type) &&
                      (stage.callInputKind == HirCallInputKind::ReadBorrow ||
                       stage.callInputKind == HirCallInputKind::MutableBorrow)
                  ? std::optional(Shape::PassiveReference)
                  : classShape(stage.info.type));
        }
        if (stage.operation != MirOperation::None) {
          const auto *mutationPlace =
              stage.destination ? mir.findPlace(*stage.destination) : nullptr;
          const bool scalarMutation =
              (stage.kind == MirInstructionKind::Compute ||
               stage.kind == MirInstructionKind::Assign) &&
              mutationPlace && isPassiveScalarValueType(mutationPlace->type) &&
              stage.operands.size() == 1 &&
              isPassiveScalarValueType(stage.operands.front().type) &&
              stage.operation >= MirOperation::AddAssign &&
              stage.operation <= MirOperation::ShiftRightAssign;
          if ((stage.operation == MirOperation::Equal ||
               stage.operation == MirOperation::NotEqual) &&
              std::any_of(stage.operands.begin(), stage.operands.end(),
                          [](const auto &operand) {
                            return operand.type.kind == SemanticType::Array;
                          }))
            add(Shape::ArrayValueComparison);
          const bool arrayAggregate =
              stage.operation == MirOperation::Aggregate &&
              stage.info.type.kind == SemanticType::Array &&
              passiveArrayElement(stage.info.type);
          const auto *addressPlace =
              stage.operands.size() == 1
                  ? mir.findPlace(stage.operands[0].place)
                  : nullptr;
          const bool arrayAddress =
              stage.operation == MirOperation::AddressOf && addressPlace &&
              arrayProjection(mir, *addressPlace);
          const bool referenceAddress =
              stage.operation == MirOperation::AddressOf && addressPlace &&
              passiveReference(addressPlace->type);
          const bool textIndex =
              stage.operation == MirOperation::Index &&
              stage.operands.size() == 2 &&
              stage.operands[0].type.kind == SemanticType::StringView &&
              stage.info.type == SemanticType::Char;
          add(stage.operation,
              stage.operation == MirOperation::PayloadConstruct ||
                      stage.operation == MirOperation::PayloadExtract
                  ? classShape(mirInstructionEnumType(stage))
              : scalarMutation ? std::optional(Shape::InPlaceScalarMutation)
              : primitiveRawIndex(stage) || primitivePointerArithmetic(stage)
                  ? std::optional(Shape::PrimitiveRawAccess)
              : arrayAggregate ? std::optional(Shape::PassiveArrayValue)
              : stage.operation == MirOperation::Aggregate &&
                      stage.info.type.kind == SemanticType::Array
                  ? classShape(stage.info.type)
              : textIndex        ? std::optional(Shape::CountedText)
              : referenceAddress ? std::optional(Shape::PassiveReference)
              : arrayAddress     ? std::optional(Shape::CheckedArrayProjection)
              : stage.operation == MirOperation::AddressOf && addressPlace
                  ? placeClassShape(*addressPlace)
                  : std::nullopt);
        }
        if (stage.intrinsic != IntrinsicKind::None) {
          const bool textQuery =
              stage.receiver &&
              stage.receiver->type.kind == SemanticType::StringView &&
              stage.operands.empty() &&
              (stage.intrinsic == IntrinsicKind::StringViewSize ||
               stage.intrinsic == IntrinsicKind::StringViewEmpty);
          add(stage.intrinsic,
              erasedReference(stage) ? std::optional(Shape::ErasedReference)
              : passiveTextViewAddress(stage) || passiveCStringAddress(stage)
                  ? std::optional(Shape::CountedText)
              : passiveRawReference(stage)
                  ? std::optional(Shape::PassiveReference)
              : stage.intrinsic == IntrinsicKind::RawPointerConversion &&
                      stage.operands.size() == 1 &&
                      passiveAddress(stage.info.type) &&
                      passiveAddress(stage.operands[0].type)
                  ? std::optional(Shape::PassiveAddressValue)
              : textQuery ? std::optional(Shape::CountedText)
              : stage.receiver &&
                      stage.receiver->type.kind == SemanticType::Array &&
                      passiveArrayElement(stage.receiver->type) &&
                      stage.operands.empty() &&
                      (stage.intrinsic == IntrinsicKind::ArraySize ||
                       stage.intrinsic == IntrinsicKind::ArrayRangeBegin)
                  ? std::optional(Shape::PassiveArrayValue)
                  : std::nullopt);
        }
        if (stage.receiver)
          operand(*stage.receiver);
        for (const auto &input : stage.operands)
          operand(input);
        for (const auto &type : stage.parameterTypes)
          this->type(type);
        for (const auto &type : stage.closureCaptureTypes)
          this->type(type);
        for (auto mode : stage.closureCaptureModes)
          add(mode);
        if (stage.objectLayoutType)
          type(*stage.objectLayoutType);
        if (stage.nativeCalleeType) {
          type(*stage.nativeCalleeType);
          add(Shape::NativeCall);
          add(Shape::IndirectCall);
        }
        if (stage.dispatch == CallDispatch::Virtual)
          add(Shape::VirtualCall);
        if (stage.functionTarget || stage.constructorTarget ||
            stage.lambdaTarget)
          add(stage.dispatch);
        if (stage.constructorTarget)
          add(stage.constructorKind, classShape(stage.info.type));
        if (stage.lambdaTarget || stage.functionPointerFunctionTarget ||
            stage.functionPointerLambdaTarget)
          add(Shape::IndirectCall);
        if (stage.nativeCallbackAdapter)
          add(Shape::NativeCallback);
        if (stage.threadTaskAdapter)
          add(Shape::ThreadTask);
        if (!stage.scopedThreadTasks.empty())
          add(Shape::ScopedThreadTasks);
        if (stage.preparedParameterDrop)
          add(Shape::PreparedOwnerInput,
              classShape(mir.findDropObligation(*stage.preparedParameterDrop)
                             ->dropType.type));
        if (stage.successResultDrop || stage.successResultDestination)
          add(Shape::SuccessResultPublication, classShape(stage.info.type));
        if (stage.tracksCaller || stage.callerLocation ||
            stage.sourceLocation || stage.forwardsCallerLocation)
          add(Shape::CallerLocation);
        bool rawAccess =
            passiveRawReference(stage) || primitiveRawIndex(stage) ||
            (stage.kind == MirInstructionKind::Borrow &&
             stage.operands.size() == 1 &&
             stage.operands[0].kind == MirOperandKind::Loan &&
             passiveReference(stage.info.type)) ||
            (stage.destination &&
             primitiveRawAccess(mir, *mir.findPlace(*stage.destination)));
        for (const auto &operand : stage.operands)
          if (const auto *place = mir.findPlace(operand.place))
            rawAccess |= primitiveRawAccess(mir, *place);
        if (stage.rawMemoryAccess)
          add(Shape::RawMemory, rawAccess
                                    ? std::optional(Shape::PrimitiveRawAccess)
                                    : std::nullopt);
        // Lifecycle marks repeat the source unsafe context but execute no
        // memory operation. The typed access/arithmetic stages are inventoried
        // independently; their pending shapes cannot be hidden by this marker.
        const auto &instructionCoverage = coverage(stage.kind);
        const bool permissionOnly =
            stage.kind == MirInstructionKind::Lifecycle ||
            (instructionCoverage.state ==
                 LlvmCoverageState::VerifiedNonExecuting &&
             instructionCoverage.nonExecuting ==
                 LlvmNonExecutingRule::VerifiedMetadata);
        if (stage.unsafeOperation != UnsafeOperationKind::None &&
            !permissionOnly) {
          const auto *callee =
              stage.functionTarget
                  ? program.findFunctionInstance(*stage.functionTarget)
                  : nullptr;
          // Caller permission never turns an ordinary source function into an
          // intrinsic. Its exact body is in the required dependency closure
          // and every unsupported operation there still rejects admission.
          const bool ordinaryUnsafe =
              stage.kind == MirInstructionKind::Call &&
              stage.intrinsic == IntrinsicKind::None && callee &&
              callee->definitionKind == MirDefinitionKind::Source &&
              callee->linkage == LanguageLinkage::Gti &&
              callee->requiresUnsafe &&
              stage.unsafeOperation == UnsafeOperationKind::DeclaredUnsafeCall;
          add(stage.unsafeOperation,
              stage.unsafeOperation == UnsafeOperationKind::UnionMember
                  ? classShape(stage.info.type).has_value() ? classShape(stage.info.type)
                    : [&]() -> std::optional<Shape> {
                        if (stage.destination)
                          return placeClassShape(*mir.findPlace(*stage.destination));
                        for (const auto &operand : stage.operands)
                          if (const auto *place = mir.findPlace(operand.place))
                            if (auto shape = placeClassShape(*place)) return shape;
                        return {};
                      }()
              : ordinaryUnsafe ? std::optional(Shape::OrdinaryUnsafeInvocation)
              : stage.unsafeOperation ==
                          UnsafeOperationKind::ErasedReferenceRestore &&
                      (erasedReference(stage) ||
                       restoredLoanTransport(mir, stage))
                  ? std::optional(Shape::ErasedReference)
              : ((stage.unsafeOperation ==
                      UnsafeOperationKind::RawDereference &&
                  (passiveTextViewAddress(stage) ||
                   passiveCStringAddress(stage))) ||
                 (stage.unsafeOperation ==
                      UnsafeOperationKind::DeclaredUnsafeCall &&
                  passiveRawReference(stage)) ||
                 (rawAccess &&
                  (stage.unsafeOperation == UnsafeOperationKind::RawIndex ||
                   stage.unsafeOperation ==
                       UnsafeOperationKind::RawDereference)) ||
                 ((primitivePointerArithmetic(stage) ||
                   (stage.kind == MirInstructionKind::Assign &&
                    passiveAddress(stage.info.type))) &&
                  stage.unsafeOperation ==
                      UnsafeOperationKind::PointerArithmetic))
                  ? std::optional(Shape::PrimitiveRawAccess)
                  : std::nullopt);
        }
        if (stage.payloadExtractionKind)
          add(*stage.payloadExtractionKind,
              classShape(mirInstructionEnumType(stage)));
        if (stage.callableBoundary)
          add(*stage.callableBoundary);
        if (stage.callableInvocation) {
          const auto *target =
              stage.functionTarget
                  ? program.findFunctionInstance(*stage.functionTarget)
                  : nullptr;
          add(*stage.callableInvocation,
              target && target->owner && !target->staticMember &&
                      !stage.lambdaTarget
                  ? classShape(program.findClassInstance(*target->owner)->type)
                  : std::nullopt);
        }
        if (stage.synchronization.kind != SynchronizationOperationKind::None) {
          add(stage.synchronization.kind);
          if (stage.synchronization.order)
            add(*stage.synchronization.order);
          if (stage.synchronization.failureOrder)
            add(*stage.synchronization.failureOrder);
        }
        if (!stage.definedFailure.empty()) {
          add(Shape::CheckedFailure);
          add(stage.definedFailure.propagation, storageShape);
          for (const auto &origin : stage.definedFailure.localOrigins)
            for (const auto &outcome : origin.outcomes) {
              const bool arrayBound =
                  outcome.code == DefinedFailureCode::IndexOutOfBounds &&
                  outcome.detail == DefinedFailureDetail::FixedArray;
              const bool textBound =
                  outcome.code == DefinedFailureCode::IndexOutOfBounds &&
                  outcome.detail == DefinedFailureDetail::StringView;
              const auto shape =
                  arrayBound  ? std::optional(Shape::CheckedArrayProjection)
                  : textBound ? std::optional(Shape::CountedText)
                              : storageShape;
              add(outcome.code, shape);
              add(outcome.detail, shape);
            }
        }
        for (const auto &event : stage.lifecycle)
          lifecycle(event);
        if (stage.elementCleanup)
          cleanup(*stage.elementCleanup);
        if (stage.replacementCleanup)
          cleanup(*stage.replacementCleanup);
        for (const auto &proof : stage.safetyDischarges)
          add(proof.family);
        if (stage.literalProvenance.kind != MirLiteralProvenanceKind::None)
          add(stage.literalProvenance.kind);
      }
      instruction = 0;
      const auto found = spans.find({block, 0});
      source = found == spans.end() ? row.identity.source : *found->second;
      add(mirBlock.terminator.kind);
      if (mirBlock.terminator.kind == MirTerminatorKind::Invoke)
        add(Shape::FailureReturn);
      if (mirBlock.terminator.value)
        operand(*mirBlock.terminator.value);
      for (const auto &argument : mirBlock.terminator.targetArguments)
        operand(argument);
      for (const auto &argument : mirBlock.terminator.elseArguments)
        operand(argument);
      for (const auto &target : mirBlock.terminator.switchTargets)
        for (const auto &argument : target.arguments)
          operand(argument);
      for (const auto &event : mirBlock.terminator.successLifecycle)
        lifecycle(event);
      for (const auto &parameter : mirBlock.parameters)
        type(parameter.type);
    }
  }

private:
  using Key = std::tuple<std::string_view, std::string_view, bool, MirBodyKind,
                         std::size_t, bool, bool, const LlvmCoverageEntry *>;
  std::vector<LlvmCoverageUse> &uses;
  const MirProgram &program;
  std::map<Key, std::size_t> indices;
};

} // namespace

LlvmAdmissionPlan::LlvmAdmissionPlan(VerifiedLoweredProgram program,
                                     LlvmBackendOptions options)
    : input(std::move(program)), policy(options) {}

const LlvmCoverageUse *LlvmAdmissionPlan::firstUnsupported() const {
  const LlvmCoverageUse *fallback = nullptr;
  for (const auto &use : uses) {
    if (!use.required)
      continue;
    const auto &entry = *use.entry;
    const bool unsupported =
        (entry.state == LlvmCoverageState::Pending &&
         (!use.implementedShape ||
          use.implementedShape->state != LlvmCoverageState::Implemented)) ||
        (use.executable &&
         entry.nonExecuting == LlvmNonExecutingRule::MustBeAbsent);
    if (!unsupported)
      continue;
    if (fallback == nullptr)
      fallback = &use;
    if (use.body && use.instruction != 0 && !use.source.source.empty())
      return &use;
  }
  return fallback;
}

std::string LlvmAdmissionPlan::limitation() const {
  const auto *use = firstUnsupported();
  if (use == nullptr)
    return {};
  std::ostringstream message;
  message << "executable LLVM lowering is not implemented yet for "
          << use->entry->key() << " (O-LLVM-" << std::setfill('0')
          << std::setw(2) << use->entry->milestone << ')';
  if (use->body)
    message << " in " << coverage(use->body->kind).name << '/'
            << use->body->owner;
  if (use->block != 0)
    message << ", block " << use->block;
  if (use->instruction != 0)
    message << ", instruction " << use->instruction;
  return message.str();
}

LlvmAdmissionPlan planLlvmAdmission(VerifiedLoweredProgram program,
                                    LlvmBackendOptions options) {
  LlvmAdmissionPlan plan(std::move(program), options);
  const auto &input = plan.program();
  const auto &mir = input.mir();
  const auto closure = native_llvm::reachableProgram(input);
  plan.closure_ = closure.conservative
                      ? LlvmAdmissionClosure::AllExecutableBodies
                      : LlvmAdmissionClosure::SealedEntryDependencies;
  Inventory inventory(plan.uses, mir);
  inventory.add(Shape::ObjectArtifact);
  inventory.add(input.programKind());
  inventory.add(mir.executionProfile());
  for (const auto &body : input.bodies()) {
    const auto *bodyMir = findMirBody(mir, body.identity.address);
    if (bodyMir == nullptr)
      throw std::logic_error("verified LLVM input lost an inventoried body");
    if (body.role == LoweredBodyRole::SourceExecutable &&
        closure.contains(body.identity.address))
      plan.bodies.push_back(body.identity.address);
    inventory.inspect(body, *bodyMir, closure.contains(body.identity.address));
  }
  inventory.body.reset();
  inventory.block = 0;
  inventory.instruction = 0;
  inventory.required = true;
  inventory.executable = true;
  // Until class/payload execution is admitted, their type requirements on
  // reachable values reject them. Dormant type inventories remain metadata.
  for (const auto &instance : mir.classInstances()) {
    inventory.required = closure.conservative;
    const auto *row = input.findClassInstance(instance.id);
    inventory.source = row == nullptr ? SourceSpan{} : row->source;
    inventory.type(instance.type);
    inventory.traits(instance.traits, instance.type);
    inventory.add(instance.kind);
    if (instance.compilerCapability != CompilerCapabilityTypeKind::None)
      inventory.add(instance.compilerCapability);
    if (!instance.declaredFields.empty())
      inventory.add(Shape::ClassFields, inventory.classShape(instance.type));
    for (const auto &field : instance.declaredFields)
      inventory.type(field.type);
    if (!instance.bases.empty() || !instance.structuralBases.empty())
      inventory.add(Shape::ClassBases);
    if (instance.polymorphic)
      inventory.add(Shape::PolymorphicClass);
    if (instance.cAbiLayout)
      inventory.add(Shape::NativeRecord, inventory.classShape(instance.type));
    if (instance.unionLayout)
      inventory.add(Shape::NativeUnion, inventory.classShape(instance.type));
    if (instance.cOpaqueHandle)
      inventory.add(Shape::OpaqueNativeHandle);
  }
  for (const auto &function : mir.functionInstances()) {
    inventory.body = MirBodyAddress{MirBodyKind::Function, function.id};
    inventory.required = closure.contains(*inventory.body);
    if (const auto *row = input.findBody(*inventory.body))
      inventory.source = row->identity.source;
    inventory.add(function.definitionKind);
    inventory.add(function.linkage);
    inventory.add(function.entryKind);
    if (function.owner)
      inventory.add(
          function.receiverMutability,
          inventory.classShape(mir.findClassInstance(*function.owner)->type));
    inventory.type(function.returnType);
    for (const auto &type : function.parameterTypes)
      inventory.type(type);
    for (const auto &parameter : function.callableParameters) {
      inventory.type(parameter.callableType);
      for (const auto &signature : parameter.signatures) {
        inventory.type(signature.returnType);
        for (const auto &type : signature.parameterTypes)
          inventory.type(type);
        inventory.add(signature.requiredCapability);
        if (signature.selectedCapability)
          inventory.add(*signature.selectedCapability);
        if (signature.nativeCalleeType)
          inventory.type(*signature.nativeCalleeType);
      }
    }
    if (function.cThreadOperation != CThreadRuntimeOperation::None)
      inventory.add(function.cThreadOperation);
    if (function.processMemoryOperation != ProcessMemoryRuntimeOperation::None)
      inventory.add(function.processMemoryOperation);
    if (function.definitionKind == MirDefinitionKind::RuntimeBinding)
      inventory.add(Shape::RuntimeBinding);
    if (function.definitionKind == MirDefinitionKind::Declaration)
      inventory.add(Shape::LibraryDeclaration);
    if (function.linkage == LanguageLinkage::C)
      inventory.add(Shape::NativeCall);
    if (function.mayRaiseDefinedFailure)
      inventory.add(Shape::FailureReturn);
    if (function.receiverMutability != ReceiverMutability::ReadOnly)
      inventory.add(Shape::MutableReceiver,
                    function.owner
                        ? inventory.classShape(
                              mir.findClassInstance(*function.owner)->type)
                        : std::nullopt);
  }
  inventory.body.reset();
  inventory.required = true;
  for (const auto &storage : mir.foreignStorageDeclarations()) {
    inventory.type(storage.type);
    inventory.add(Shape::NativeCall);
  }
  for (const auto &payload : mir.payloadEnums()) {
    inventory.required = closure.conservative;
    inventory.type(payload.type);
    inventory.add(Shape::PayloadEnum);
    for (const auto &variant : payload.variants)
      for (const auto &type : variant.payloadTypes)
        inventory.type(type);
  }
  for (const auto &storage : mir.threadLocalStorage()) {
    inventory.required = true;
    inventory.body =
        MirBodyAddress{MirBodyKind::ThreadLocalInitializer, storage.symbol};
    if (const auto *row = input.findBody(*inventory.body))
      inventory.source = row->identity.source;
    inventory.type(storage.type);
    inventory.add(storage.dataInitializer ? Shape::ThreadLocalPassive
                                          : Shape::ThreadLocalDynamic,
                  inventory.nativeValue(storage.type)
                      ? std::optional(Shape::NativeThreadStorage)
                      : std::nullopt);
    if (storage.requiresActiveCleanup)
      inventory.cleanup(storage.cleanup);
  }
  inventory.body.reset();
  inventory.required = true;
  inventory.source = {};
  for (const auto &step : mir.programInitializationPlan().steps) {
    // Unused passive data is not executable initialization. Reachable symbol
    // accesses and exports still require an admitted storage representation.
    inventory.required =
        closure.conservative ||
        step.role == ProgramInitializationStepRole::Initializer;
    if (inventory.required)
      inventory.add(Shape::ProgramInitialization);
    inventory.add(step.dataInitialization);
    inventory.add(step.storageKind);
    inventory.add(step.role);
  }
  inventory.required = true;
  if (mir.hostedStartupPlan())
    for (const auto &operation : mir.hostedStartupPlan()->operations) {
      inventory.add(operation.kind);
      inventory.add(operation.failureBehavior);
    }
  if (input.embeddingPlan())
    inventory.add(Shape::EmbeddingExports);
  for (const auto &callback : mir.nativeCallbackAdapters()) {
    inventory.required = closure.conservative;
    inventory.add(Shape::NativeCallback);
    inventory.add(callback.cThreadCallback);
  }
  for (const auto &item : input.generatedItems()) {
    inventory.required = closure.contains(item.identity);
    std::optional<Shape> shape;
    if (item.identity.kind == LoweredGeneratedItemKind::ThreadLocalStorage)
      if (const auto *storage = mir.findThreadLocalStorage(item.identity.owner))
        if (inventory.nativeValue(storage->type))
          shape = Shape::NativeThreadStorage;
    if (const auto *cleanup =
            std::get_if<LoweredLifecycleCleanupItem>(&item.payload))
      if (const auto *owner = mir.findClassInstance(cleanup->classInstance))
        shape = inventory.classShape(owner->type);
    if (const auto *adapter =
            std::get_if<LoweredConcreteInstanceAdapterItem>(&item.payload))
      if (adapter->ownerClassInstance)
        if (const auto *owner =
                mir.findClassInstance(adapter->ownerClassInstance))
          shape = inventory.classShape(owner->type);
    inventory.add(item.identity.kind, shape);
    inventory.add(item.sourceKind, shape);
    if (!item.dependencies.empty())
      inventory.add(Shape::GeneratedDependencies, shape);
  }
  // Declarations describe the source surface too, including uninstantiated
  // generics. Inventory them without demanding executable representations.
  inventory.required = false;
  inventory.executable = false;
  for (const auto &declaration : input.declarations()) {
    inventory.source = declaration.source;
    inventory.add(declaration.kind);
  }
  return plan;
}

} // namespace lang
