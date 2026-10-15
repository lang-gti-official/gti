#include "gti/mir_constant_evaluator.h"
#include "gti/mir_verifier.h"
#include "mir_verification_queries.h"

#include <algorithm>

namespace lang {
namespace {

bool scalarType(const SemanticType &type) {
  return constantIntegerDomain(type).has_value() ||
         semanticFloatFormat(type).has_value() || type == SemanticType::Bool ||
         type == SemanticType::Char || type == SemanticType::StringView ||
         type == SemanticType::NullPtr;
}

std::optional<TokenKind> operatorToken(MirOperation operation) {
  switch (operation) {
  case MirOperation::Add:
  case MirOperation::AddAssign:
    return TokenKind::PLUS;
  case MirOperation::Subtract:
  case MirOperation::SubtractAssign:
    return TokenKind::MINUS;
  case MirOperation::Multiply:
  case MirOperation::MultiplyAssign:
    return TokenKind::STAR;
  case MirOperation::Divide:
  case MirOperation::DivideAssign:
    return TokenKind::SLASH;
  case MirOperation::Remainder:
  case MirOperation::RemainderAssign:
    return TokenKind::PERCENT;
  case MirOperation::BitwiseAnd:
  case MirOperation::BitwiseAndAssign:
    return TokenKind::AMPERSAND;
  case MirOperation::BitwiseOr:
  case MirOperation::BitwiseOrAssign:
    return TokenKind::PIPE;
  case MirOperation::BitwiseXor:
  case MirOperation::BitwiseXorAssign:
    return TokenKind::CARET;
  case MirOperation::ShiftLeft:
  case MirOperation::ShiftLeftAssign:
    return TokenKind::SHIFT_LEFT;
  case MirOperation::ShiftRight:
  case MirOperation::ShiftRightAssign:
    return TokenKind::SHIFT_RIGHT;
  case MirOperation::Equal:
    return TokenKind::EQUAL_EQUAL;
  case MirOperation::NotEqual:
    return TokenKind::BANG_EQUAL;
  case MirOperation::Less:
    return TokenKind::LESS;
  case MirOperation::LessEqual:
    return TokenKind::LESS_EQUAL;
  case MirOperation::Greater:
    return TokenKind::GREATER;
  case MirOperation::GreaterEqual:
    return TokenKind::GREATER_EQUAL;
  case MirOperation::Positive:
    return TokenKind::PLUS;
  case MirOperation::Negate:
    return TokenKind::MINUS;
  case MirOperation::LogicalNot:
    return TokenKind::BANG;
  case MirOperation::BitwiseNot:
    return TokenKind::TILDE;
  default:
    return std::nullopt;
  }
}

class Evaluation {
public:
  Evaluation(const MirProgram &program, MirConstantLimits limits)
      : program(program), limits(limits) {}

  MirConstantEvaluation run(MirBodyAddress root,
                            std::span<const ConstantValue> arguments,
                            const std::optional<ConstantValue> &receiver) {
    const auto verification = verifyMirProgram(program);
    if (!verification.valid()) {
      return {.failure = ConstantEvaluationFailure::UnsupportedExpression,
              .function = root.kind == MirBodyKind::Function ? root.owner : 0,
              .body = root,
              .detail = "Constant execution requires verified MIR: " +
                        verification.errors.front().message};
    }
    for (const auto &argument : arguments) {
      if (const auto failure = persistentValue(argument);
          failure != ConstantEvaluationFailure::None)
        return {.failure = failure,
                .body = root,
                .detail =
                    "A constant request cannot import transient ownership."};
    }
    if (receiver) {
      if (const auto failure = persistentValue(*receiver);
          failure != ConstantEvaluationFailure::None)
        return {.failure = failure, .body = root};
    }
    auto result = root.kind == MirBodyKind::Constructor
                      ? construct(root.owner, arguments, 0)
                      : invoke(root.owner, arguments, 0, receiver);
    const auto *function = root.kind == MirBodyKind::Function
                               ? program.findFunctionInstance(root.owner)
                               : nullptr;
    const auto *constructor = root.kind == MirBodyKind::Constructor
                                  ? program.findConstructorInstance(root.owner)
                                  : nullptr;
    const auto *owner =
        constructor ? program.findClassInstance(constructor->owner) : nullptr;
    const auto type = function ? observedType(function->returnType)
                      : owner  ? owner->type
                               : SemanticType::Unknown;
    if (result && type != SemanticType::Void && !persistentType(type)) {
      result.value.reset();
      result.failure = ConstantEvaluationFailure::UnsupportedType;
      result.detail = "An object requiring cleanup cannot escape constant "
                      "execution; return a passive value instead.";
    }
    if (result && result.value) {
      const auto failure = persistentValue(*result.value);
      if (failure != ConstantEvaluationFailure::None) {
        result.value.reset();
        result.failure = failure;
        result.detail =
            "A constant result must not retain transient ownership.";
      }
    }
    if (result &&
        std::any_of(heap.begin(), heap.end(),
                    [](const auto &allocation) { return allocation->live; })) {
      result.value.reset();
      result.voidResult = false;
      result.failure = ConstantEvaluationFailure::UnsupportedExpression;
      result.detail = "Every constant allocation must be destroyed before "
                      "constant evaluation finishes.";
    }
    result.steps = steps;
    return result;
  }

private:
  ConstantEvaluationFailure persistentValue(const ConstantValue &value,
                                            std::size_t depth = 0) {
    const auto *node = std::get_if<ConstantAggregateValue>(&value);
    if (!node)
      return ConstantEvaluationFailure::None;
    if (depth >= limits.callDepth || !charge(1))
      return ConstantEvaluationFailure::ResourceLimit;
    if (!node->value ||
        node->value->kind == ConstantAggregateKind::TransientOwner)
      return ConstantEvaluationFailure::InvalidOperands;
    for (const auto &item : node->value->elements)
      if (const auto failure = persistentValue(item, depth + 1);
          failure != ConstantEvaluationFailure::None)
        return failure;
    for (const auto &field : node->value->fields)
      if (const auto failure = persistentValue(field.value, depth + 1);
          failure != ConstantEvaluationFailure::None)
        return failure;
    return ConstantEvaluationFailure::None;
  }

  bool persistentType(const SemanticType &type, std::size_t depth = 0) const {
    if (depth >= limits.callDepth || type.kind == SemanticType::UniqueOwner ||
        type.kind == SemanticType::Reference)
      return false;
    if (type.kind == SemanticType::Class) {
      const auto *instance = record(type);
      if (!instance || instance->requiresActiveCleanup || instance->destructor)
        return false;
      return std::all_of(instance->declaredFields.begin(),
                         instance->declaredFields.end(),
                         [&](const auto &field) {
                           return persistentType(field.type, depth + 1);
                         });
    }
    return std::all_of(type.arguments.begin(), type.arguments.end(),
                       [&](const auto &argument) {
                         return persistentType(argument, depth + 1);
                       });
  }

  const SemanticType &observedType(const SemanticType &type) const {
    return type.kind == SemanticType::Reference && type.arguments.size() == 1
               ? type.arguments.front()
               : type;
  }

  bool observedStorageType(const SemanticType &type) const {
    return storageType(observedType(type));
  }

  const MirClassInstance *record(const SemanticType &type) const {
    for (const auto &instance : program.classInstances()) {
      if (instance.type == type)
        return &instance;
    }
    return nullptr;
  }

  bool storageType(const SemanticType &type, std::size_t depth = 0) const {
    if (scalarType(type))
      return true;
    if (type.kind == SemanticType::Enum) {
      const auto *enumeration = program.findIntegralEnum(type);
      return enumeration &&
             constantIntegerDomain(enumeration->underlyingType).has_value();
    }
    if (depth >= limits.callDepth)
      return false;
    if (type.kind == SemanticType::UniqueOwner)
      return type.arguments.size() == 1 &&
             storageType(type.arguments[0], depth + 1);
    if (type.kind == SemanticType::TypePack)
      return type.concretePack &&
             std::all_of(type.arguments.begin(), type.arguments.end(),
                         [&](const auto &element) {
                           return storageType(element, depth + 1);
                         });
    if (type.kind == SemanticType::Expected)
      return type.arguments.size() == 2 &&
             (type.arguments[0] == SemanticType::Void ||
              storageType(type.arguments[0], depth + 1)) &&
             storageType(type.arguments[1], depth + 1);
    if (type.kind == SemanticType::Unexpected)
      return type.arguments.size() == 1 &&
             storageType(type.arguments[0], depth + 1);
    if (type.kind == SemanticType::Array)
      return type.arguments.size() == 1 && type.arrayLengthParameterId == 0 &&
             storageType(type.arguments[0], depth + 1);
    if (type.kind != SemanticType::Class)
      return false;
    const auto *instance = record(type);
    if (!instance ||
        instance->compilerCapability != CompilerCapabilityTypeKind::None ||
        instance->kind == ClassKind::Union || instance->polymorphic ||
        !instance->bases.empty() || !instance->structuralBases.empty() ||
        instance->lifecycleBoundBorrowCarrier ||
        instance->traits.containsBorrowedState)
      return false;
    return std::all_of(instance->declaredFields.begin(),
                       instance->declaredFields.end(),
                       [&](const MirClassFieldInfo &field) {
                         return storageType(field.type, depth + 1);
                       });
  }

  struct Frame;
  struct RetiredProjection {
    MirPlaceRootKind root;
    std::size_t cell;
    std::size_t epoch;
    std::vector<MirPlaceProjection> path;
  };
  struct Allocation {
    SemanticType type;
    std::optional<ConstantValue> value;
    std::size_t epoch = 1;
    bool live = true;
    std::size_t units = 1;
    std::vector<RetiredProjection> retired;
  };
  // A checked reference designates an existing constant cell and a resolved
  // field/index path, never a target address. Frames live throughout nested
  // calls; an internal returned reference must designate an enclosing frame.
  // Resolving indices at formation preserves the source's once-formed place.
  struct Location {
    Frame *frame = nullptr;
    MirPlaceRootKind root = MirPlaceRootKind::Binding;
    std::size_t cell = 0;
    std::size_t epoch = 0;
    std::vector<MirPlaceProjection> path;
    const SemanticType *type = nullptr;
    AccessMode access = AccessMode::ReadOnly;
    std::size_t allocation = 0;
  };

  struct Loan {
    Location location;
    bool active = true;
  };

  struct Frame {
    const MirBody &body;
    MirBodyAddress address;
    std::vector<std::optional<ConstantValue>> values;
    std::vector<std::optional<ConstantValue>> places;
    std::vector<std::optional<Loan>> loans;
    std::vector<std::optional<Location>> references;
    std::vector<std::optional<Location>> valueReferences;
    std::vector<std::size_t> placeEpochs;
    std::vector<std::size_t> valueEpochs;
    std::vector<bool> liveDrops;
    std::vector<RetiredProjection> retired;
    MirBlockId block = 0;
    MirInstructionId instruction = 0;
    std::optional<ConstantValue> receiver;
    std::optional<Location> receiverOrigin;
    AccessMode receiverAccess = AccessMode::ReadOnly;
    std::size_t receiverEpoch = 1;
  };

  MirConstantEvaluation fail(const Frame &frame,
                             ConstantEvaluationFailure failure,
                             std::string detail = {}) const {
    return {.failure = failure,
            .function = frame.address.kind == MirBodyKind::Function
                            ? frame.address.owner
                            : 0,
            .body = frame.address,
            .block = frame.block,
            .instruction = frame.instruction,
            .detail = std::move(detail)};
  }

  // Different MIR projections of one binding may have distinct place IDs.
  // Projections share the canonical whole-root cell. Immutable aggregate
  // values are replaced along the indexed path on assignment, so copying an
  // array never creates a mutable alias to another binding.
  MirPlaceId cell(const Frame &frame, MirPlaceId id) const {
    const auto *place = frame.body.findPlace(id);
    if (place == nullptr || !observedStorageType(place->type) ||
        (place->root != MirPlaceRootKind::Binding &&
         place->root != MirPlaceRootKind::Temporary)) {
      return 0;
    }
    for (const auto &candidate : frame.body.places) {
      if (candidate.root == place->root && candidate.projections.empty() &&
          candidate.binding == place->binding &&
          candidate.temporary == place->temporary &&
          observedStorageType(candidate.type)) {
        return candidate.id;
      }
    }
    return 0;
  }

  std::optional<ConstantValue> &storage(const Location &location) {
    if (location.allocation)
      return heap[location.allocation - 1]->value;
    if (location.root == MirPlaceRootKind::This)
      return location.frame->receiver;
    if (location.root == MirPlaceRootKind::Value)
      return location.frame->values[location.cell];
    return location.frame->places[location.cell];
  }

  std::size_t &epoch(const Location &location) {
    if (location.allocation)
      return heap[location.allocation - 1]->epoch;
    if (location.root == MirPlaceRootKind::This)
      return location.frame->receiverEpoch;
    if (location.root == MirPlaceRootKind::Value)
      return location.frame->valueEpochs[location.cell];
    return location.frame->placeEpochs[location.cell];
  }

  std::vector<RetiredProjection> &retired(const Location &location) {
    return location.allocation ? heap[location.allocation - 1]->retired
                               : location.frame->retired;
  }

  static bool pathPrefix(std::span<const MirPlaceProjection> left,
                         std::span<const MirPlaceProjection> right) {
    return left.size() <= right.size() &&
           std::equal(left.begin(), left.end(), right.begin());
  }

  ConstantEvaluation locate(Frame &frame, MirPlaceId id, Location &location,
                            bool writing = false) {
    const auto *place = frame.body.findPlace(id);
    if (!place || !observedStorageType(place->type))
      return {.failure = ConstantEvaluationFailure::UnsupportedType};
    const MirPlace *binding = nullptr;
    if (place->root == MirPlaceRootKind::Loan) {
      if (place->loan >= frame.loans.size() || !frame.loans[place->loan])
        return {.failure = ConstantEvaluationFailure::NonConstantReference};
      const auto &loan = *frame.loans[place->loan];
      if (writing && !loan.active)
        return {.failure = ConstantEvaluationFailure::NonConstantReference};
      if (!charge(loan.location.path.size()))
        return {.failure = ConstantEvaluationFailure::ResourceLimit};
      location = loan.location;
    } else if (place->root == MirPlaceRootKind::This) {
      if (frame.receiverOrigin) {
        if (!charge(frame.receiverOrigin->path.size()))
          return {.failure = ConstantEvaluationFailure::ResourceLimit};
        location = *frame.receiverOrigin;
      } else {
        const auto *aggregate =
            frame.receiver
                ? std::get_if<ConstantAggregateValue>(&*frame.receiver)
                : nullptr;
        const auto *owner =
            aggregate && aggregate->value && aggregate->value->exactType
                ? record(*aggregate->value->exactType)
                : nullptr;
        if (!owner)
          return {.failure = ConstantEvaluationFailure::UnsupportedType};
        location = {.frame = &frame,
                    .root = MirPlaceRootKind::This,
                    .epoch = frame.receiverEpoch,
                    .type = &owner->type,
                    .access = frame.receiverAccess};
      }
    } else if (place->root == MirPlaceRootKind::Value) {
      if (place->value == 0 || place->value >= frame.values.size())
        return {.failure = ConstantEvaluationFailure::NonConstantReference};
      if (place->value < frame.valueReferences.size() &&
          frame.valueReferences[place->value]) {
        if (!charge(frame.valueReferences[place->value]->path.size()))
          return {.failure = ConstantEvaluationFailure::ResourceLimit};
        location = *frame.valueReferences[place->value];
      } else {
        location = {.frame = &frame,
                    .root = MirPlaceRootKind::Value,
                    .cell = place->value,
                    .epoch = frame.valueEpochs[place->value],
                    .type = &observedType(
                        frame.body.findValue(place->value)->info.type),
                    .access = place->access};
      }
    } else {
      const auto root = cell(frame, id);
      if (!root)
        return {.failure = ConstantEvaluationFailure::UnsupportedExpression};
      binding = frame.body.findPlace(root);
      if (root < frame.references.size() && frame.references[root]) {
        if (!charge(frame.references[root]->path.size()))
          return {.failure = ConstantEvaluationFailure::ResourceLimit};
        location = *frame.references[root];
      } else {
        location = {.frame = &frame,
                    .cell = root,
                    .epoch = frame.placeEpochs[root],
                    .type = &observedType(binding->type),
                    .access = binding->type.kind == SemanticType::Reference
                                  ? binding->type.referenceAccess
                                  : binding->access};
      }
    }
    // Root/loan/receiver permission and field declarations determine what
    // may be written. MirPlace::access also describes a particular expression's
    // read access; it is not the maximum permission of a checked referent.
    const SemanticType *projectedType = location.type;
    for (std::size_t index = 0; index < place->projections.size(); ++index) {
      if (!charge(1))
        return {.failure = ConstantEvaluationFailure::ResourceLimit};
      auto projection = place->projections[index];
      if (projection.kind == MirProjectionKind::Dereference && index == 0 &&
          binding && binding->type.kind == SemanticType::Reference)
        continue;
      if (projection.kind == MirProjectionKind::Dereference && projectedType &&
          projectedType->kind == SemanticType::UniqueOwner) {
        location.type = projectedType;
        const auto owner = readLocation(location);
        if (!owner)
          return owner;
        Location target;
        const auto object =
            pointee(*owner.value, *projectedType, target, location.access);
        if (!object)
          return object;
        location = std::move(target);
        projectedType = location.type;
        continue;
      }
      if (projection.kind == MirProjectionKind::PackElement) {
        if (!projectedType || projectedType->kind != SemanticType::TypePack ||
            !projectedType->concretePack || !projection.constantIndex ||
            *projection.constantIndex >= projectedType->arguments.size())
          return {.failure = ConstantEvaluationFailure::InvalidOperands};
        projectedType = &projectedType->arguments[*projection.constantIndex];
        projection.kind = MirProjectionKind::Index;
      } else if (projection.kind == MirProjectionKind::Index) {
        if (!projectedType || projectedType->kind != SemanticType::Array ||
            projectedType->arguments.size() != 1)
          return {.failure = ConstantEvaluationFailure::UnsupportedType};
        projectedType = &projectedType->arguments.front();
        if (projection.index != 0) {
          if (projection.index >= frame.values.size() ||
              !frame.values[projection.index])
            return {.failure = ConstantEvaluationFailure::NonConstantReference};
          projection.constantIndex =
              constantUnsignedMagnitude(*frame.values[projection.index]);
          projection.index = 0;
        }
        if (!projection.constantIndex)
          return {.failure = ConstantEvaluationFailure::InvalidOperands};
      } else if (projection.kind == MirProjectionKind::Field) {
        const auto *owner = projectedType ? record(*projectedType) : nullptr;
        if (!owner)
          return {.failure = ConstantEvaluationFailure::UnsupportedType};
        const auto field = std::find_if(
            owner->declaredFields.begin(), owner->declaredFields.end(),
            [&](const auto &candidate) {
              return candidate.symbol == projection.field;
            });
        if (field == owner->declaredFields.end())
          return {.failure = ConstantEvaluationFailure::InvalidOperands};
        projectedType = &field->type;
        if (field->access == AccessMode::ReadOnly)
          location.access = AccessMode::ReadOnly;
      } else {
        return {.failure = ConstantEvaluationFailure::UnsupportedExpression};
      }
      location.path.push_back(projection);
    }
    location.type = &observedType(place->type);
    // locate's success has no value; callers distinguish it by failure.
    return {};
  }

  ConstantEvaluation locateOperand(Frame &frame, const MirOperand &input,
                                   Location &location) {
    if (input.kind == MirOperandKind::Loan) {
      if (input.loan >= frame.loans.size() || !frame.loans[input.loan])
        return {.failure = ConstantEvaluationFailure::NonConstantReference};
      const auto &loan = *frame.loans[input.loan];
      if (input.place) {
        const auto result = locate(frame, input.place, location);
        if (result.failure != ConstantEvaluationFailure::None)
          return result;
      } else {
        if (!charge(loan.location.path.size()))
          return {.failure = ConstantEvaluationFailure::ResourceLimit};
        location = loan.location;
      }
      if (loan.location.access == AccessMode::ReadOnly || !loan.active)
        location.access = AccessMode::ReadOnly;
      return {};
    }
    if (input.place)
      return locate(frame, input.place, location,
                    input.kind == MirOperandKind::BorrowWrite);
    if (input.kind == MirOperandKind::Value && input.value != 0 &&
        input.value < frame.values.size()) {
      if (input.value < frame.valueReferences.size() &&
          frame.valueReferences[input.value]) {
        if (!charge(frame.valueReferences[input.value]->path.size()))
          return {.failure = ConstantEvaluationFailure::ResourceLimit};
        location = *frame.valueReferences[input.value];
      } else {
        location = {.frame = &frame,
                    .root = MirPlaceRootKind::Value,
                    .cell = input.value,
                    .epoch = frame.valueEpochs[input.value],
                    .type = &observedType(input.type)};
      }
      return {};
    }
    return {.failure = ConstantEvaluationFailure::UnsupportedExpression};
  }

  bool charge(std::size_t amount) {
    if (amount > limits.steps - std::min(steps, limits.steps))
      return false;
    steps += amount;
    return true;
  }

  ConstantValue ownerValue(const SemanticType &type, std::size_t allocation) {
    return makeConstantAggregate(
        {.kind = ConstantAggregateKind::TransientOwner,
         .identity = allocation,
         .exactType = std::make_shared<const SemanticType>(type)});
  }

  ConstantEvaluation convertValue(const ConstantValue &value,
                                  const SemanticType &type) {
    if (type.kind == SemanticType::TypePack) {
      const auto *node = std::get_if<ConstantAggregateValue>(&value);
      if (!node || !node->value || !node->value->exactType ||
          *node->value->exactType != type || !type.concretePack ||
          node->value->kind != ConstantAggregateKind::Array ||
          node->value->elements.size() != type.arguments.size())
        return {.failure = ConstantEvaluationFailure::InvalidOperands};
      return {.value = value};
    }
    if (type.kind != SemanticType::UniqueOwner)
      return convertConstantToType(value, type);
    if (std::holds_alternative<NullConstant>(value))
      return {.value = ownerValue(type, 0)};
    const auto *node = std::get_if<ConstantAggregateValue>(&value);
    if (!node || !node->value || !node->value->exactType ||
        *node->value->exactType != type ||
        node->value->kind != ConstantAggregateKind::TransientOwner ||
        !node->value->elements.empty() || !node->value->fields.empty() ||
        node->value->identity > heap.size())
      return {.failure = ConstantEvaluationFailure::InvalidOperands};
    if (node->value->identity && !heap[node->value->identity - 1]->live)
      return {.failure = ConstantEvaluationFailure::NonConstantReference};
    return {.value = value};
  }

  ConstantEvaluation allocate(const SemanticType &owner,
                              std::optional<ConstantValue> value = {}) {
    if (owner.kind != SemanticType::UniqueOwner || !storageType(owner))
      return {.failure = ConstantEvaluationFailure::UnsupportedType};
    if (!charge(1) || heap.size() >= limits.allocations)
      return {.failure = ConstantEvaluationFailure::ResourceLimit};
    std::size_t units = 0;
    if (value) {
      const auto failure = measureValue(*value, units);
      if (failure != ConstantEvaluationFailure::None)
        return {.failure = failure};
    } else {
      units = 1;
    }
    if (units > limits.heapUnits - std::min(heapUnits, limits.heapUnits))
      return {.failure = ConstantEvaluationFailure::ResourceLimit};
    heapUnits += units;
    heap.push_back(
        std::make_unique<Allocation>(Allocation{.type = owner.arguments[0],
                                                .value = std::move(value),
                                                .units = units}));
    return {.value = ownerValue(owner, heap.size())};
  }

  ConstantEvaluationFailure measureValue(const ConstantValue &value,
                                         std::size_t &units,
                                         std::size_t depth = 0) {
    if (depth >= limits.callDepth || !charge(1) || units >= limits.heapUnits)
      return ConstantEvaluationFailure::ResourceLimit;
    ++units;
    if (const auto *text = std::get_if<std::string>(&value)) {
      if (text->size() > limits.heapUnits - units)
        return ConstantEvaluationFailure::ResourceLimit;
      units += text->size();
    } else if (const auto *node = std::get_if<ConstantAggregateValue>(&value)) {
      if (!node->value)
        return ConstantEvaluationFailure::InvalidOperands;
      for (const auto &element : node->value->elements)
        if (const auto failure = measureValue(element, units, depth + 1);
            failure != ConstantEvaluationFailure::None)
          return failure;
      for (const auto &field : node->value->fields)
        if (const auto failure = measureValue(field.value, units, depth + 1);
            failure != ConstantEvaluationFailure::None)
          return failure;
    }
    return ConstantEvaluationFailure::None;
  }

  ConstantEvaluation storeAllocation(std::size_t id, ConstantValue value) {
    auto &allocation = *heap[id - 1];
    std::size_t units = 0;
    const auto failure = measureValue(value, units);
    if (failure != ConstantEvaluationFailure::None)
      return {.failure = failure};
    const auto retained = heapUnits - allocation.units;
    if (units > limits.heapUnits - std::min(retained, limits.heapUnits))
      return {.failure = ConstantEvaluationFailure::ResourceLimit};
    heapUnits = retained + units;
    allocation.units = units;
    allocation.value = value;
    return {.value = std::move(value)};
  }

  ConstantEvaluation pointee(const ConstantValue &owner,
                             const SemanticType &type, Location &location,
                             AccessMode access) {
    const auto checked = convertValue(owner, type);
    if (!checked)
      return checked;
    const auto id =
        std::get<ConstantAggregateValue>(*checked.value).value->identity;
    if (!id || !heap[id - 1]->value)
      return {.failure = ConstantEvaluationFailure::NonConstantReference};
    location = {.epoch = heap[id - 1]->epoch,
                .type = &heap[id - 1]->type,
                .access = access,
                .allocation = id};
    return readLocation(location);
  }

  ConstantEvaluation exactValue(const ConstantValue &value,
                                const SemanticType &type) {
    if (!charge(1))
      return {.failure = ConstantEvaluationFailure::ResourceLimit};
    const auto converted = convertValue(value, type);
    if (!converted)
      return converted;
    if (*converted.value != value)
      return {.failure = ConstantEvaluationFailure::InvalidOperands};
    if (const auto *enumerator = std::get_if<ConstantEnum>(&value)) {
      const auto *enumeration = program.findIntegralEnum(type);
      if (!enumeration ||
          constantIntegerDomain(enumeration->underlyingType) !=
              enumerator->value.domain ||
          !checkedIntegerFits(checkedIntegerValue(enumerator->value),
                              enumerator->value.domain) ||
          (enumerator->value.negative && enumerator->value.magnitude == 0))
        return {.failure = ConstantEvaluationFailure::InvalidOperands};
    }
    const auto *aggregate = std::get_if<ConstantAggregateValue>(&value);
    if (!aggregate)
      return converted;
    if (type.kind == SemanticType::UniqueOwner)
      return converted;
    if (type.kind == SemanticType::TypePack) {
      for (std::size_t index = 0; index < type.arguments.size(); ++index) {
        const auto valid = exactValue(aggregate->value->elements[index],
                                      type.arguments[index]);
        if (!valid)
          return valid;
      }
    } else if (type.kind == SemanticType::Array) {
      for (const auto &element : aggregate->value->elements) {
        const auto valid = exactValue(element, type.arguments[0]);
        if (!valid)
          return valid;
      }
    } else if (type.kind == SemanticType::Expected ||
               type.kind == SemanticType::Unexpected) {
      if (!aggregate->value->elements.empty()) {
        const bool error =
            aggregate->value->kind == ConstantAggregateKind::ExpectedError;
        const auto payload = exactValue(aggregate->value->elements[0],
                                        type.arguments[error ? 1 : 0]);
        return payload ? converted : payload;
      }
    } else if (const auto *instance = record(type)) {
      if (aggregate->value->fields.size() != instance->declaredFields.size())
        return {.failure = ConstantEvaluationFailure::InvalidOperands};
      for (const auto &field : instance->declaredFields) {
        const auto &fields = aggregate->value->fields;
        const auto found =
            std::find_if(fields.begin(), fields.end(),
                         [&](const ConstantAggregateField &item) {
                           return item.symbol == field.symbol;
                         });
        if (found == fields.end())
          return {.failure = ConstantEvaluationFailure::InvalidOperands};
        const auto valid = exactValue(found->value, field.type);
        if (!valid)
          return valid;
      }
    } else {
      return {.failure = ConstantEvaluationFailure::UnsupportedType};
    }
    return converted;
  }

  ConstantEvaluation project(const ConstantValue &value,
                             std::span<const MirPlaceProjection> path,
                             const SemanticType &type, std::size_t offset,
                             const ConstantValue *replacement = nullptr) {
    if (offset == path.size()) {
      return replacement ? convertValue(*replacement, type)
                         : ConstantEvaluation{.value = value};
    }
    if (!charge(1))
      return {.failure = ConstantEvaluationFailure::ResourceLimit};
    const auto &projection = path[offset];
    if (projection.kind == MirProjectionKind::Field) {
      const auto *aggregate = std::get_if<ConstantAggregateValue>(&value);
      if (!aggregate || !aggregate->value ||
          aggregate->value->kind != ConstantAggregateKind::Object)
        return {.failure = ConstantEvaluationFailure::UnsupportedType};
      const auto &fields = aggregate->value->fields;
      const auto field = std::find_if(fields.begin(), fields.end(),
                                      [&](const ConstantAggregateField &item) {
                                        return item.symbol == projection.field;
                                      });
      if (field == fields.end())
        return {.failure = ConstantEvaluationFailure::NonConstantReference};
      auto result = project(field->value, path, type, offset + 1, replacement);
      if (!result || !replacement)
        return result;
      if (!charge(fields.size()))
        return {.failure = ConstantEvaluationFailure::ResourceLimit};
      auto updated = *aggregate->value;
      updated.fields[field - fields.begin()].value = std::move(*result.value);
      return {.value = makeConstantAggregate(std::move(updated))};
    }
    if (projection.kind != MirProjectionKind::Index)
      return {.failure = ConstantEvaluationFailure::UnsupportedExpression};
    const auto index = projection.constantIndex;
    const auto *aggregate = std::get_if<ConstantAggregateValue>(&value);
    if (!aggregate || !aggregate->value ||
        (aggregate->value->kind != ConstantAggregateKind::Array &&
         aggregate->value->kind != ConstantAggregateKind::ExpectedSuccess &&
         aggregate->value->kind != ConstantAggregateKind::ExpectedError &&
         aggregate->value->kind != ConstantAggregateKind::Unexpected))
      return {.failure = ConstantEvaluationFailure::UnsupportedType};
    if (!index || *index >= aggregate->value->elements.size())
      return {.failure = ConstantEvaluationFailure::InvalidOperands};
    auto result = project(aggregate->value->elements[*index], path, type,
                          offset + 1, replacement);
    if (!result || !replacement)
      return result;
    // Charge before copying the backing vector, including for nested updates.
    if (!charge(aggregate->value->elements.size()))
      return {.failure = ConstantEvaluationFailure::ResourceLimit};
    auto updated = *aggregate->value;
    updated.elements[*index] = std::move(*result.value);
    return {.value = makeConstantAggregate(std::move(updated))};
  }

  ConstantEvaluation readLocation(const Location &location) {
    const auto &value = storage(location);
    if (location.epoch != epoch(location) || !value)
      return {.failure = ConstantEvaluationFailure::NonConstantReference};
    for (const auto &moved : retired(location)) {
      if (moved.root == location.root && moved.cell == location.cell &&
          moved.epoch == location.epoch &&
          (pathPrefix(moved.path, location.path) ||
           pathPrefix(location.path, moved.path)))
        return {.failure = ConstantEvaluationFailure::NonConstantReference};
    }
    return project(*value, location.path, *location.type, 0);
  }

  ConstantEvaluation readPlace(Frame &frame, MirPlaceId id) {
    Location location;
    const auto located = locate(frame, id, location);
    return located.failure == ConstantEvaluationFailure::None
               ? readLocation(location)
               : located;
  }

  ConstantEvaluation retirePlace(Frame &frame, MirPlaceId id) {
    Location location;
    const auto located = locate(frame, id, location);
    if (located.failure != ConstantEvaluationFailure::None)
      return located;
    if (!location.path.empty()) {
      if (!charge(location.path.size() + 1))
        return {.failure = ConstantEvaluationFailure::ResourceLimit};
      retired(location).push_back({location.root, location.cell, location.epoch,
                                   std::move(location.path)});
      return {};
    }
    storage(location).reset();
    ++epoch(location);
    return {};
  }

  ConstantEvaluation writePlace(Frame &frame, MirPlaceId id,
                                const ConstantValue &value,
                                bool initializing = false) {
    const auto *target = frame.body.findPlace(id);
    if (target && target->root == MirPlaceRootKind::This && frame.receiver &&
        frame.address.kind == MirBodyKind::Constructor && initializing &&
        target->projections.size() == 1 &&
        target->projections[0].kind == MirProjectionKind::Field) {
      const auto *aggregate =
          std::get_if<ConstantAggregateValue>(&*frame.receiver);
      if (!aggregate || !aggregate->value)
        return {.failure = ConstantEvaluationFailure::UnsupportedType};
      const auto &fields = aggregate->value->fields;
      const auto symbol = target->projections[0].field;
      if (std::any_of(fields.begin(), fields.end(),
                      [&](const ConstantAggregateField &field) {
                        return field.symbol == symbol;
                      }))
        return {.failure = ConstantEvaluationFailure::InvalidOperands};
      const auto converted = exactValue(value, target->type);
      if (!converted)
        return converted;
      if (!charge(fields.size() + 1))
        return {.failure = ConstantEvaluationFailure::ResourceLimit};
      auto updated = *aggregate->value;
      updated.fields.push_back({symbol, value});
      frame.receiver = makeConstantAggregate(std::move(updated));
      return converted;
    }
    Location location;
    const auto located = locate(frame, id, location, true);
    if (located.failure != ConstantEvaluationFailure::None)
      return located;
    if (!initializing && location.access != AccessMode::Mutable)
      return {.failure = ConstantEvaluationFailure::UnsupportedExpression};
    return writeLocation(location, value, initializing);
  }

  ConstantEvaluation writeLocation(const Location &location,
                                   const ConstantValue &value,
                                   bool initializing = false) {
    auto &valueStorage = storage(location);
    if (location.epoch != epoch(location) ||
        (!location.path.empty() && !valueStorage))
      return {.failure = ConstantEvaluationFailure::NonConstantReference};
    // Replacing the moved place restores it; writing only a child of a moved
    // aggregate cannot resurrect the missing enclosing value.
    for (const auto &moved : retired(location))
      if (moved.root == location.root && moved.cell == location.cell &&
          moved.epoch == location.epoch &&
          moved.path.size() < location.path.size() &&
          pathPrefix(moved.path, location.path))
        return {.failure = ConstantEvaluationFailure::NonConstantReference};
    auto updated = location.path.empty() ? convertValue(value, *location.type)
                                         : project(*valueStorage, location.path,
                                                   *location.type, 0, &value);
    if (updated) {
      if (location.allocation) {
        updated =
            storeAllocation(location.allocation, std::move(*updated.value));
        if (!updated)
          return updated;
      } else {
        valueStorage = std::move(updated.value);
      }
      auto &moved = retired(location);
      std::erase_if(moved, [&](const auto &item) {
        return item.root == location.root && item.cell == location.cell &&
               item.epoch == location.epoch &&
               pathPrefix(location.path, item.path);
      });
      if (initializing && location.path.empty())
        ++epoch(location);
    }
    return updated.failure == ConstantEvaluationFailure::None
               ? ConstantEvaluation{.value = value}
               : updated;
  }

  ConstantEvaluation defaultValue(const SemanticType &type) {
    if (!charge(1))
      return {.failure = ConstantEvaluationFailure::ResourceLimit};
    if (type.kind == SemanticType::Array)
      return arrayValue(type, {});
    if (type.kind == SemanticType::Enum)
      return enumValue(type, {});
    if (type.kind == SemanticType::Expected && type.arguments.size() == 2) {
      if (type.arguments[0] == SemanticType::Void)
        return makeConstantResult(type, true);
      auto payload = defaultValue(type.arguments[0]);
      return payload ? makeConstantResult(type, true, payload.value) : payload;
    }
    if (const auto domain = constantIntegerDomain(type))
      return {.value = makeConstantInteger({}, *domain)};
    if (const auto format = semanticFloatFormat(type))
      return convertConstantFloat(
          makeConstantInteger({}, {.width = 32, .signedValue = true}), *format);
    if (type == SemanticType::Bool)
      return {.value = false};
    if (type == SemanticType::Char)
      return {.value = CharacterLiteral{0}};
    if (type == SemanticType::StringView)
      return {.value = std::string{}};
    if (type == SemanticType::NullPtr)
      return {.value = NullConstant{}};
    if (type.kind == SemanticType::UniqueOwner)
      return {.value = ownerValue(type, 0)};
    return {.failure = ConstantEvaluationFailure::UnsupportedType};
  }

  ConstantEvaluation enumValue(const SemanticType &type,
                               EnumConstant value) const {
    const auto *enumeration = program.findIntegralEnum(type);
    const auto domain = enumeration
                            ? constantIntegerDomain(enumeration->underlyingType)
                            : std::nullopt;
    if (!domain)
      return {.failure = ConstantEvaluationFailure::UnsupportedType};
    if (!checkedIntegerFits({value.negative, value.magnitude}, *domain))
      return {.failure = ConstantEvaluationFailure::InvalidOperands};
    return {.value = ConstantEnum{
                .enumId = type.enumId,
                .value = makeConstantInteger(
                    {.negative = value.negative, .magnitude = value.magnitude},
                    *domain)}};
  }

  ConstantEvaluation arrayValue(const SemanticType &type,
                                const std::vector<ConstantValue> &values) {
    if (!storageType(type) || type.kind != SemanticType::Array)
      return {.failure = ConstantEvaluationFailure::UnsupportedType};
    if (values.size() > type.arrayLength)
      return {.failure = ConstantEvaluationFailure::InvalidOperands};
    if (!charge(type.arrayLength))
      return {.failure = ConstantEvaluationFailure::ResourceLimit};
    ConstantAggregate aggregate;
    aggregate.exactType = std::make_shared<const SemanticType>(type);
    aggregate.elements.reserve(type.arrayLength);
    for (std::size_t index = 0; index < type.arrayLength; ++index) {
      auto value = index < values.size()
                       ? convertConstantToType(values[index], type.arguments[0])
                       : defaultValue(type.arguments[0]);
      if (!value)
        return value;
      aggregate.elements.push_back(std::move(*value.value));
    }
    return {.value = makeConstantAggregate(std::move(aggregate))};
  }

  ConstantEvaluation operand(Frame &frame, const MirOperand &input) {
    if (input.kind == MirOperandKind::Loan && input.loan < frame.loans.size() &&
        frame.loans[input.loan]) {
      Location location;
      const auto located = locateOperand(frame, input, location);
      return located.failure == ConstantEvaluationFailure::None
                 ? readLocation(location)
                 : located;
    }
    if (input.kind == MirOperandKind::Constant && input.literal) {
      return evaluateConstantLiteral(*input.literal,
                                     constantIntegerDomain(input.type));
    }
    if (input.kind == MirOperandKind::Value && input.value != 0 &&
        input.value < frame.values.size() && frame.values[input.value]) {
      if (input.value < frame.valueReferences.size() &&
          frame.valueReferences[input.value])
        return readLocation(*frame.valueReferences[input.value]);
      return {.value = frame.values[input.value]};
    }
    if (input.kind == MirOperandKind::Copy ||
        input.kind == MirOperandKind::Move ||
        input.kind == MirOperandKind::BorrowRead ||
        input.kind == MirOperandKind::BorrowWrite) {
      return readPlace(frame, input.place);
    }
    return {.failure = ConstantEvaluationFailure::NonConstantReference};
  }

  ConstantEvaluation compute(const MirInstruction &instruction,
                             const std::vector<ConstantValue> &values) {
    const auto &type = instruction.info.type;
    if (instruction.operation == MirOperation::EnumConstant &&
        instruction.enumOwner && instruction.enumValue &&
        type.kind == SemanticType::Enum &&
        *instruction.enumOwner == type.enumId && values.empty())
      return enumValue(type, *instruction.enumValue);
    if (instruction.operation == MirOperation::Aggregate &&
        type.kind == SemanticType::Array)
      return arrayValue(type, values);
    if (instruction.operation == MirOperation::Aggregate &&
        type.kind == SemanticType::Expected && values.empty())
      return defaultValue(type);
    if (instruction.operation == MirOperation::Unexpected && values.size() == 1)
      return makeConstantResult(type, false, values[0]);
    if (instruction.operation == MirOperation::ExpectedHasValue &&
        values.size() == 1) {
      return constantResultHasValue(values[0]);
    }
    if (instruction.operation == MirOperation::Literal && instruction.literal) {
      return evaluateConstantLiteral(*instruction.literal,
                                     constantIntegerDomain(type));
    }
    if (instruction.operation == MirOperation::Identity && values.size() == 1) {
      return {.value = values.front()};
    }
    if (instruction.operation == MirOperation::Convert && values.size() == 1) {
      return convertConstantToType(values.front(), type);
    }
    if (instruction.operation == MirOperation::Index && values.size() == 2 &&
        instruction.operands[0].type == SemanticType::StringView) {
      return evaluateConstantTextIndex(values[0], values[1]);
    }
    if (const auto operation = operatorToken(instruction.operation)) {
      if (values.size() == 1) {
        return evaluateConstantUnary(*operation, values[0],
                                     constantIntegerDomain(type));
      }
      if (values.size() == 2) {
        return evaluateConstantBinary(*operation, values[0], values[1],
                                      constantIntegerDomain(type),
                                      semanticFloatFormat(type));
      }
    }
    return {.failure = ConstantEvaluationFailure::UnsupportedExpression};
  }

  MirConstantEvaluation destroy(const MirDropType &recipe, Location location,
                                Frame &caller, std::size_t depth) {
    if (!recipe.requiresActiveCleanup)
      return {};
    if (!charge(1) || depth >= limits.callDepth)
      return fail(caller, ConstantEvaluationFailure::ResourceLimit);
    if (!storageType(recipe.type) || recipe.polymorphicCleanup ||
        recipe.recursiveCleanup || recipe.lambdaInstance)
      return fail(caller, ConstantEvaluationFailure::UnsupportedType,
                  "Cleanup is outside the constant execution profile.");
    for (const auto &moved : retired(location))
      if (moved.root == location.root && moved.cell == location.cell &&
          moved.epoch == location.epoch &&
          pathPrefix(moved.path, location.path))
        return {};
    location.access = AccessMode::Mutable;
    if (recipe.type.kind == SemanticType::UniqueOwner) {
      const auto value = readLocation(location);
      if (!value)
        return fail(caller, value.failure);
      const auto checked = convertValue(*value.value, recipe.type);
      if (!checked)
        return fail(caller, checked.failure);
      const auto id =
          std::get<ConstantAggregateValue>(*checked.value).value->identity;
      if (!id)
        return {};
      Location object;
      const auto resolved =
          pointee(*checked.value, recipe.type, object, AccessMode::Mutable);
      if (!resolved)
        return fail(caller, resolved.failure);
      if (!recipe.aggregate.empty()) {
        if (recipe.aggregate.size() != 1)
          return fail(caller, ConstantEvaluationFailure::InvalidOperands);
        auto result = destroy(recipe.aggregate[0], object, caller, depth + 1);
        if (result.failure != ConstantEvaluationFailure::None)
          return result;
      } else if (recipe.destructor) {
        const MirDropType leaf{.type = *object.type,
                               .classInstance = recipe.uniquePointeeClass,
                               .destructor = recipe.destructor,
                               .requiresActiveCleanup = true};
        auto result = destroy(leaf, object, caller, depth + 1);
        if (result.failure != ConstantEvaluationFailure::None)
          return result;
      }
      heap[id - 1]->live = false;
      heap[id - 1]->value.reset();
      heapUnits -= heap[id - 1]->units;
      heap[id - 1]->units = 0;
      ++heap[id - 1]->epoch;
      return {};
    }
    if (recipe.type.kind == SemanticType::Array) {
      if (!recipe.fixedArray || recipe.aggregate.size() != 1)
        return fail(caller, ConstantEvaluationFailure::UnsupportedType);
      for (std::size_t index = recipe.type.arrayLength; index != 0; --index) {
        auto element = location;
        element.path.push_back(
            {.kind = MirProjectionKind::Index, .constantIndex = index - 1});
        element.type = &recipe.aggregate[0].type;
        auto result =
            destroy(recipe.aggregate[0], std::move(element), caller, depth + 1);
        if (result.failure != ConstantEvaluationFailure::None)
          return result;
      }
      return {};
    }
    if (recipe.type.kind == SemanticType::Expected ||
        recipe.type.kind == SemanticType::Unexpected) {
      // State inspection does not read a payload already moved through an
      // extraction. Its recursive cleanup checks the retired payload path.
      const auto &backing = storage(location);
      const auto value =
          backing && location.epoch == epoch(location)
              ? project(*backing, location.path, *location.type, 0)
              : ConstantEvaluation{};
      if (!value)
        return fail(caller, value.failure);
      const auto *node = std::get_if<ConstantAggregateValue>(&*value.value);
      if (!node || !node->value)
        return fail(caller, ConstantEvaluationFailure::InvalidOperands);
      const std::size_t alternative =
          node->value->kind == ConstantAggregateKind::ExpectedError ? 1 : 0;
      if (alternative >= recipe.aggregate.size())
        return fail(caller, ConstantEvaluationFailure::InvalidOperands);
      if (!recipe.aggregate[alternative].requiresActiveCleanup)
        return {};
      location.path.push_back(
          {.kind = MirProjectionKind::Index, .constantIndex = 0});
      location.type = &recipe.aggregate[alternative].type;
      return destroy(recipe.aggregate[alternative], std::move(location), caller,
                     depth + 1);
    }
    if (recipe.type.kind != SemanticType::Class)
      return fail(caller, ConstantEvaluationFailure::UnsupportedType);
    if (recipe.destructor) {
      const auto *destructor =
          program.findDestructorInstance(*recipe.destructor);
      const auto value = readLocation(location);
      if (!destructor ||
          destructor->definitionKind != MirDefinitionKind::Source || !value)
        return fail(caller, ConstantEvaluationFailure::UnsupportedExpression,
                    "Constant destruction requires an available source body.");
      auto result = executeBody(destructor->body,
                                {MirBodyKind::Destructor, destructor->id},
                                SemanticType::Void, {}, {}, {}, depth + 1,
                                value.value, {}, location, AccessMode::Mutable);
      if (!result)
        return result;
    }
    // The verifier owns the ordered, exact subobject recipe. Do not reconstruct
    // field order or destructor selection from a native representation.
    for (const auto &child : recipe.aggregate) {
      if (!child.uniqueOwnerField || child.uniqueOwnerBase ||
          child.captureIndex)
        return fail(caller, ConstantEvaluationFailure::UnsupportedType);
      auto field = location;
      field.path.push_back(
          {.kind = MirProjectionKind::Field, .field = child.uniqueOwnerField});
      field.type = &child.type;
      auto result = destroy(child, std::move(field), caller, depth + 1);
      if (result.failure != ConstantEvaluationFailure::None)
        return result;
    }
    return {};
  }

  void applyLifecycle(Frame &frame, std::span<const MirLifecycleEvent> events) {
    for (const auto &event : events) {
      switch (event.kind) {
      case MirLifecycleEventKind::Initialize:
      case MirLifecycleEventKind::Replace:
        if (event.target)
          frame.liveDrops[event.target] = true;
        break;
      case MirLifecycleEventKind::Move:
      case MirLifecycleEventKind::Reparent:
        if (event.source)
          frame.liveDrops[event.source] = false;
        if (event.target)
          frame.liveDrops[event.target] = true;
        break;
      case MirLifecycleEventKind::TransferOut:
      case MirLifecycleEventKind::Drop:
        if (event.source)
          frame.liveDrops[event.source] = false;
        break;
      }
    }
  }

  MirConstantEvaluation execute(Frame &frame, const MirInstruction &instruction,
                                std::size_t depth) {
    // The ordinary verifier owns loan conflicts and endpoints. Execution
    // retains their exact constant-cell origins rather than freezing copies.
    if (instruction.kind == MirInstructionKind::EndBorrow && instruction.loan) {
      frame.loans[*instruction.loan].reset();
      return {};
    }
    if (instruction.kind == MirInstructionKind::ActivateBorrow &&
        instruction.loan) {
      auto &loan = frame.loans[*instruction.loan];
      if (!loan)
        return fail(frame, ConstantEvaluationFailure::NonConstantReference);
      loan->active = true;
      return {};
    }
    if (instruction.rawMemoryAccess ||
        instruction.unsafeOperation != UnsafeOperationKind::None ||
        instruction.bodyTarget || instruction.lambdaTarget ||
        (instruction.callableInvocation &&
         ((*instruction.callableInvocation !=
               CallableInvocationCapability::Read &&
           *instruction.callableInvocation !=
               CallableInvocationCapability::Mutable) ||
          instruction.kind != MirInstructionKind::Call ||
          !instruction.functionTarget ||
          instruction.dispatch != CallDispatch::Static))) {
      return fail(frame, ConstantEvaluationFailure::UnsupportedExpression,
                  "Operation is outside the constant MIR execution profile.");
    }
    if (instruction.kind == MirInstructionKind::Lifecycle) {
      return {};
    }
    if (instruction.kind == MirInstructionKind::AllocateObject) {
      if (!instruction.destination || !instruction.objectAllocation)
        return fail(frame, ConstantEvaluationFailure::InvalidOperands);
      const auto owner = allocate(instruction.info.type);
      if (!owner)
        return fail(frame, owner.failure);
      const auto stored =
          writePlace(frame, *instruction.destination, *owner.value, true);
      return stored ? MirConstantEvaluation{} : fail(frame, stored.failure);
    }
    if (instruction.kind == MirInstructionKind::CleanupBeforeAssign) {
      if (!instruction.destination || !instruction.replacementCleanup ||
          !storageType(instruction.replacementCleanup->type))
        return fail(frame, ConstantEvaluationFailure::UnsupportedType);
      // Verification pairs this passive preparation with its assignment.
      // Preserve the aggregate shell; the assignment replaces the value and
      // retires the applicable generation. Bounds are checked at preparation.
      Location location;
      const auto located = locate(frame, *instruction.destination, location);
      if (located.failure != ConstantEvaluationFailure::None)
        return fail(frame, located.failure);
      if (!instruction.replacementCleanup->requiresActiveCleanup) {
        const auto checked = readLocation(location);
        return checked ? MirConstantEvaluation{} : fail(frame, checked.failure);
      }
      return destroy(*instruction.replacementCleanup, std::move(location),
                     frame, depth + 1);
    }
    if (instruction.kind == MirInstructionKind::Drop) {
      for (const auto &event : instruction.lifecycle) {
        if (event.kind != MirLifecycleEventKind::Drop || !event.source)
          continue;
        const auto *obligation = frame.body.findDropObligation(event.source);
        if (!obligation)
          return fail(frame, ConstantEvaluationFailure::InvalidOperands);
        if (obligation->dropType.requiresActiveCleanup &&
            !frame.liveDrops[event.source]) {
          // Normal MIR may retire the shell of a moved parameter as well as
          // conditionally guarded failure cleanup. Neither owns resources now.
          return {};
        }
        // Retire the obligation before entering user cleanup, matching the
        // ordinary MIR failure/lifetime contract.
        frame.liveDrops[event.source] = false;
        Location location;
        const auto located = locate(frame, obligation->place, location);
        if (located.failure != ConstantEvaluationFailure::None)
          return fail(frame, located.failure);
        auto result = destroy(obligation->dropType, std::move(location), frame,
                              depth + 1);
        if (result.failure != ConstantEvaluationFailure::None)
          return result;
      }
      if (instruction.destination) {
        const auto retired = retirePlace(frame, *instruction.destination);
        if (retired.failure != ConstantEvaluationFailure::None)
          return fail(frame, retired.failure);
      }
      return {};
    }
    // Checked place formation can be staged before a later index/argument
    // without reading or copying the selected object at runtime. Canonical
    // MIR spells that check as an unused, safe AddressOf. Validate the same
    // aggregate projection here without manufacturing a constant pointer.
    // Used addresses and all unsafe/raw-memory operations remain excluded.
    if (instruction.kind == MirInstructionKind::Compute &&
        instruction.operation == MirOperation::AddressOf &&
        instruction.result && frame.body.usesOf(*instruction.result).empty() &&
        instruction.operands.size() == 1 &&
        instruction.operands.front().kind == MirOperandKind::Address) {
      const auto *place =
          frame.body.findPlace(instruction.operands.front().place);
      if (place != nullptr && !place->projections.empty() &&
          place->projections.back().kind == MirProjectionKind::Index) {
        const auto checked = readPlace(frame, place->id);
        return checked ? MirConstantEvaluation{} : fail(frame, checked.failure);
      }
    }
    if (!observedStorageType(instruction.info.type) &&
        !(instruction.kind == MirInstructionKind::Call &&
          instruction.info.type == SemanticType::Void)) {
      return fail(frame, ConstantEvaluationFailure::UnsupportedType);
    }
    // The positive magnitude/identity chain for a signed-minimum literal has
    // no standalone value in its final signed type. Retire that exact chain
    // and publish only the fused literal, just as native MIR emission does.
    if (instruction.kind == MirInstructionKind::Compute) {
      if (const auto *literal =
              mir_detail::fusedSignedMinimumLiteral(frame.body, instruction)) {
        const auto magnitude = std::get<std::uint64_t>(*literal->literal);
        frame.values[*instruction.result] =
            makeConstantInteger({.negative = true, .magnitude = magnitude},
                                *constantIntegerDomain(instruction.info.type));
        ++frame.valueEpochs[*instruction.result];
        return {};
      }
      if ((instruction.operation == MirOperation::Literal ||
           instruction.operation == MirOperation::Identity) &&
          mir_detail::fusedSignedMinimumNegation(frame.body, instruction)) {
        return {};
      }
    }
    std::vector<ConstantValue> arguments;
    for (const auto &input : instruction.operands) {
      auto value = operand(frame, input);
      if (!value) {
        return fail(frame, value.failure);
      }
      arguments.push_back(std::move(*value.value));
    }
    ConstantEvaluation evaluated;
    std::optional<Location> reference;
    bool voidResult = false;
    switch (instruction.kind) {
    case MirInstructionKind::Borrow:
      if (instruction.loan && arguments.size() == 1) {
        Location location;
        const auto located =
            locateOperand(frame, instruction.operands.front(), location);
        if (located.failure != ConstantEvaluationFailure::None)
          return fail(frame, located.failure);
        const auto *loan = frame.body.findLoan(*instruction.loan);
        if (!loan || (loan->access == AccessMode::Mutable &&
                      location.access != AccessMode::Mutable))
          return fail(frame, ConstantEvaluationFailure::InvalidOperands);
        location.access = loan->access;
        frame.loans[*instruction.loan] =
            Loan{std::move(location), !loan->twoPhase};
        evaluated.value = arguments.front();
      }
      break;
    case MirInstructionKind::Compute:
      evaluated = compute(instruction, arguments);
      break;
    case MirInstructionKind::Load:
    case MirInstructionKind::Move:
    case MirInstructionKind::CallInput:
      if (arguments.size() == 1) {
        if (instruction.kind == MirInstructionKind::CallInput &&
            (instruction.callInputKind == HirCallInputKind::ReadBorrow ||
             instruction.callInputKind == HirCallInputKind::MutableBorrow ||
             instruction.callInputKind ==
                 HirCallInputKind::ReadTemporaryBorrow ||
             instruction.callInputKind ==
                 HirCallInputKind::MutableTemporaryBorrow)) {
          Location location;
          const auto located =
              locateOperand(frame, instruction.operands.front(), location);
          if (located.failure != ConstantEvaluationFailure::None)
            return fail(frame, located.failure);
          if (instruction.callInputKind == HirCallInputKind::ReadBorrow ||
              instruction.callInputKind ==
                  HirCallInputKind::ReadTemporaryBorrow)
            location.access = AccessMode::ReadOnly;
          reference = std::move(location);
        }
        evaluated.value = arguments.front();
      }
      break;
    case MirInstructionKind::Initialize:
    case MirInstructionKind::Assign:
      if (arguments.size() == 1 && instruction.destination) {
        const auto *target = frame.body.findPlace(*instruction.destination);
        if (instruction.kind == MirInstructionKind::Initialize && target &&
            target->type.kind == SemanticType::Reference &&
            target->projections.empty()) {
          Location location;
          const auto located =
              locateOperand(frame, instruction.operands.front(), location);
          if (located.failure != ConstantEvaluationFailure::None)
            return fail(frame, located.failure);
          const auto root = cell(frame, target->id);
          if (!root || (target->type.referenceAccess == AccessMode::Mutable &&
                        location.access != AccessMode::Mutable))
            return fail(frame, ConstantEvaluationFailure::InvalidOperands);
          location.access = target->type.referenceAccess;
          if (frame.references.empty())
            frame.references.resize(frame.places.size());
          frame.references[root] = std::move(location);
          evaluated.value = arguments.front();
          break;
        }
        if (instruction.kind == MirInstructionKind::Assign &&
            operatorToken(instruction.operation)) {
          const auto before = readPlace(frame, *instruction.destination);
          if (!before)
            return fail(frame, before.failure);
          evaluated = evaluateConstantBinary(
              *operatorToken(instruction.operation), *before.value,
              arguments.front(), constantIntegerDomain(instruction.info.type),
              semanticFloatFormat(instruction.info.type));
        } else {
          evaluated = convertValue(arguments.front(),
                                   observedType(instruction.info.type));
        }
        if (evaluated)
          evaluated =
              writePlace(frame, *instruction.destination, *evaluated.value,
                         instruction.kind == MirInstructionKind::Initialize);
      }
      break;
    case MirInstructionKind::Modify: {
      const auto loaded = instruction.destination
                              ? readPlace(frame, *instruction.destination)
                              : ConstantEvaluation{};
      if (!loaded)
        return fail(frame, loaded.failure);
      const ConstantValue before = *loaded.value;
      const auto operation = instruction.operation;
      const bool increment = operation == MirOperation::PreIncrement ||
                             operation == MirOperation::PostIncrement;
      const bool decrement = operation == MirOperation::PreDecrement ||
                             operation == MirOperation::PostDecrement;
      if ((increment || decrement) && arguments.empty()) {
        evaluated = evaluateConstantBinary(
            increment ? TokenKind::PLUS : TokenKind::MINUS, before,
            makeConstantInteger({.magnitude = 1},
                                {.width = 32, .signedValue = true}),
            constantIntegerDomain(instruction.info.type),
            semanticFloatFormat(instruction.info.type));
      } else if (arguments.size() == 1 && operatorToken(operation)) {
        evaluated = evaluateConstantBinary(
            *operatorToken(operation), before, arguments.front(),
            constantIntegerDomain(instruction.info.type),
            semanticFloatFormat(instruction.info.type));
      }
      if (evaluated) {
        evaluated =
            writePlace(frame, *instruction.destination, *evaluated.value);
        if (!evaluated)
          return fail(frame, evaluated.failure);
        if (operation == MirOperation::PostIncrement ||
            operation == MirOperation::PostDecrement)
          evaluated.value = before;
      }
      break;
    }
    case MirInstructionKind::Construct:
    case MirInstructionKind::Call: {
      if (instruction.kind == MirInstructionKind::Call &&
          !(instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner &&
            instruction.objectAllocation))
        break;
      if (!instruction.constructorTarget || instruction.receiver)
        return fail(frame, ConstantEvaluationFailure::UnsupportedExpression);
      const auto *callee =
          program.findConstructorInstance(*instruction.constructorTarget);
      if (!callee || callee->parameterTypes.size() != arguments.size())
        return fail(frame, ConstantEvaluationFailure::InvalidOperands);
      std::vector<std::optional<Location>> origins(arguments.size());
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto &type = callee->parameterTypes[index];
        if (type.kind != SemanticType::Reference)
          continue;
        Location location;
        const auto located =
            locateOperand(frame, instruction.operands[index], location);
        if (located.failure != ConstantEvaluationFailure::None)
          return fail(frame, located.failure);
        if (type.referenceAccess == AccessMode::Mutable &&
            location.access != AccessMode::Mutable)
          return fail(frame, ConstantEvaluationFailure::InvalidOperands);
        location.access = type.referenceAccess;
        origins[index] = std::move(location);
      }
      auto result = construct(*instruction.constructorTarget, arguments,
                              depth + 1, origins);
      if (!result)
        return result;
      if (instruction.objectAllocation) {
        const auto *allocation =
            frame.body.findDropObligation(instruction.objectAllocation);
        const auto owner = allocation ? readPlace(frame, allocation->place)
                                      : ConstantEvaluation{};
        if (!owner)
          return fail(frame, ConstantEvaluationFailure::InvalidOperands);
        const auto checked = convertValue(*owner.value, instruction.info.type);
        if (!checked)
          return fail(frame, checked.failure);
        const auto id =
            std::get<ConstantAggregateValue>(*checked.value).value->identity;
        if (!id || heap[id - 1]->value)
          return fail(frame, ConstantEvaluationFailure::InvalidOperands);
        const auto stored = storeAllocation(id, std::move(*result.value));
        if (!stored)
          return fail(frame, stored.failure);
        evaluated.value = checked.value;
      } else {
        evaluated.value = std::move(result.value);
      }
      break;
    }
    default:
      return fail(frame, ConstantEvaluationFailure::UnsupportedExpression,
                  "Instruction is outside the constant MIR profile.");
    }
    if (instruction.kind == MirInstructionKind::Call &&
        !instruction.objectAllocation) {
      if (instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner) {
        const auto &type = instruction.info.type;
        if (type.kind != SemanticType::UniqueOwner ||
            type.arguments.size() != 1 || arguments.size() > 1 ||
            type.arguments[0].kind == SemanticType::Class)
          return fail(frame, ConstantEvaluationFailure::UnsupportedExpression);
        const auto value = arguments.empty()
                               ? defaultValue(type.arguments[0])
                               : convertValue(arguments[0], type.arguments[0]);
        evaluated = value ? allocate(type, value.value) : value;
      } else if (instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
                 instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrowMut) {
        if (arguments.size() != 1)
          return fail(frame, ConstantEvaluationFailure::InvalidOperands);
        Location location;
        evaluated = pointee(
            arguments[0], observedType(instruction.operands[0].type), location,
            instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrowMut
                ? AccessMode::Mutable
                : AccessMode::ReadOnly);
        if (evaluated)
          reference = std::move(location);
      } else if (instruction.intrinsic == IntrinsicKind::UniqueOwnerIsNull) {
        if (arguments.size() != 1)
          return fail(frame, ConstantEvaluationFailure::InvalidOperands);
        const auto owner = convertValue(
            arguments[0], observedType(instruction.operands[0].type));
        if (!owner)
          return fail(frame, owner.failure);
        evaluated.value =
            std::get<ConstantAggregateValue>(*owner.value).value->identity == 0;
      } else if (instruction.intrinsic == IntrinsicKind::UniqueOwnerSwap) {
        if (arguments.size() != 2)
          return fail(frame, ConstantEvaluationFailure::InvalidOperands);
        Location left, right;
        const auto a = locateOperand(frame, instruction.operands[0], left);
        const auto b = locateOperand(frame, instruction.operands[1], right);
        if (a.failure != ConstantEvaluationFailure::None ||
            b.failure != ConstantEvaluationFailure::None ||
            left.access != AccessMode::Mutable ||
            right.access != AccessMode::Mutable)
          return fail(frame, ConstantEvaluationFailure::InvalidOperands);
        const auto l = writeLocation(left, arguments[1]);
        const auto r = l ? writeLocation(right, arguments[0]) : l;
        if (!r)
          return fail(frame, r.failure);
        voidResult = true;
      } else if (instruction.receiver &&
                 (instruction.intrinsic == IntrinsicKind::ExpectedValue ||
                  instruction.intrinsic == IntrinsicKind::ExpectedError ||
                  instruction.intrinsic == IntrinsicKind::ExpectedValueOr)) {
        const auto result = operand(frame, *instruction.receiver);
        if (!result)
          return fail(frame, result.failure);
        const auto state = constantResultHasValue(*result.value);
        if (!state)
          return fail(frame, state.failure);
        const bool success = std::get<bool>(*state.value);
        if (instruction.intrinsic == IntrinsicKind::ExpectedValueOr) {
          if (arguments.size() != 1)
            return fail(frame, ConstantEvaluationFailure::InvalidOperands);
          evaluated = success ? constantResultPayload(*result.value, true)
                              : convertConstantToType(arguments[0],
                                                      instruction.info.type);
        } else if (!arguments.empty() ||
                   success != (instruction.intrinsic ==
                               IntrinsicKind::ExpectedValue)) {
          return fail(frame, ConstantEvaluationFailure::InvalidOperands,
                      "Constant evaluation cannot extract the inactive result "
                      "alternative.");
        } else if (instruction.info.type == SemanticType::Void) {
          voidResult = true;
        } else {
          evaluated = constantResultPayload(*result.value, success);
          if (evaluated &&
              (instruction.loan ||
               instruction.info.category == ValueCategory::Place ||
               instruction.info.type.kind == SemanticType::Reference)) {
            Location location;
            const auto located =
                locateOperand(frame, *instruction.receiver, location);
            if (located.failure != ConstantEvaluationFailure::None)
              return fail(frame, located.failure);
            location.path.push_back(
                {.kind = MirProjectionKind::Index, .constantIndex = 0});
            location.type = &observedType(instruction.info.type);
            reference = std::move(location);
          }
        }
      } else if (const auto arithmetic =
                     integerArithmeticIntrinsic(instruction.intrinsic);
                 arithmetic && !instruction.receiver && arguments.size() == 2) {
        const bool checked =
            arithmetic->mode == IntegerArithmeticMode::CheckedResult;
        const auto &type = instruction.info.type;
        const auto domain =
            constantIntegerDomain(checked ? type.arguments[0] : type);
        const auto *left = std::get_if<ConstantInteger>(&arguments[0]);
        const auto *right = std::get_if<ConstantInteger>(&arguments[1]);
        if (!domain || !left || !right || left->domain != *domain ||
            right->domain != *domain)
          return fail(frame, ConstantEvaluationFailure::InvalidOperands);
        if (checked) {
          const auto value = evaluateCheckedIntegerBinary(
              arithmetic->operation, checkedIntegerValue(*left),
              checkedIntegerValue(*right), *domain);
          if (!value)
            return fail(frame, ConstantEvaluationFailure::InvalidOperands);
          if (const auto *integer = std::get_if<CheckedIntegerValue>(&*value)) {
            evaluated = makeConstantResult(
                type, true, makeConstantInteger(*integer, *domain));
          } else if (std::get<CheckedIntegerFailure>(*value) ==
                     CheckedIntegerFailure::Overflow) {
            const auto error = enumValue(type.arguments[1], {});
            if (!error)
              return fail(frame, error.failure);
            evaluated = makeConstantResult(type, false, error.value);
          }
        } else {
          const auto value = evaluateDefinedIntegerBinary(
              arithmetic->operation, checkedIntegerValue(*left),
              checkedIntegerValue(*right), *domain, arithmetic->mode);
          if (value)
            evaluated.value = makeConstantInteger(*value, *domain);
        }
      } else if (instruction.intrinsic == IntrinsicKind::ArraySize &&
                 instruction.receiver && arguments.empty() &&
                 instruction.receiver->type.kind == SemanticType::Array) {
        // Observe the represented receiver, not merely a type spelling: a
        // runtime/uninitialized array is not a constant by virtue of its size.
        const auto array = operand(frame, *instruction.receiver);
        if (!array)
          return fail(frame, array.failure);
        evaluated.value = makeConstantInteger(
            {.magnitude = instruction.receiver->type.arrayLength},
            {.width = 64, .signedValue = false});
      } else if (instruction.functionTarget &&
                 instruction.intrinsic == IntrinsicKind::None) {
        const auto *callee =
            program.findFunctionInstance(*instruction.functionTarget);
        if (!callee || callee->parameterTypes.size() != arguments.size())
          return fail(frame, ConstantEvaluationFailure::InvalidOperands);
        std::vector<std::optional<Location>> origins(arguments.size());
        for (std::size_t index = 0; index < arguments.size(); ++index) {
          if (callee->parameterTypes[index].kind != SemanticType::Reference)
            continue;
          Location location;
          const auto located =
              locateOperand(frame, instruction.operands[index], location);
          if (located.failure != ConstantEvaluationFailure::None)
            return fail(frame, located.failure);
          if (callee->parameterTypes[index].referenceAccess ==
                  AccessMode::Mutable &&
              location.access != AccessMode::Mutable)
            return fail(frame, ConstantEvaluationFailure::InvalidOperands);
          location.access = callee->parameterTypes[index].referenceAccess;
          origins[index] = std::move(location);
        }
        std::optional<ConstantValue> receiver;
        std::optional<Location> receiverOrigin;
        if (instruction.receiver) {
          auto value = operand(frame, *instruction.receiver);
          if (!value)
            return fail(frame, value.failure);
          receiver = std::move(value.value);
          Location location;
          const auto located =
              locateOperand(frame, *instruction.receiver, location);
          if (located.failure != ConstantEvaluationFailure::None)
            return fail(frame, located.failure);
          if (callee->receiverMutability == ReceiverMutability::Mutable &&
              location.access != AccessMode::Mutable)
            return fail(frame, ConstantEvaluationFailure::InvalidOperands);
          location.access =
              callee->receiverMutability == ReceiverMutability::Mutable
                  ? AccessMode::Mutable
                  : AccessMode::ReadOnly;
          receiverOrigin = std::move(location);
        }
        auto result = invoke(*instruction.functionTarget, arguments, depth + 1,
                             std::move(receiver), origins,
                             std::move(receiverOrigin), &reference);
        if (!result)
          return result;
        evaluated.value = std::move(result.value);
        voidResult = result.voidResult;
      } else if (instruction.receiver && arguments.empty() &&
                 (instruction.intrinsic == IntrinsicKind::StringViewSize ||
                  instruction.intrinsic == IntrinsicKind::StringViewEmpty)) {
        const auto text = operand(frame, *instruction.receiver);
        if (!text)
          return fail(frame, text.failure);
        evaluated = instruction.intrinsic == IntrinsicKind::StringViewSize
                        ? evaluateConstantTextSize(*text.value)
                        : evaluateConstantTextEmpty(*text.value);
      } else if (!instruction.receiver && arguments.empty() &&
                 instruction.intrinsic ==
                     IntrinsicKind::DefaultTypeParameterConstruction) {
        evaluated = defaultValue(instruction.info.type);
      } else if (!instruction.receiver && arguments.size() == 1 &&
                 (instruction.intrinsic ==
                      IntrinsicKind::NumericAliasConversion ||
                  instruction.intrinsic ==
                      IntrinsicKind::NumericTypeParameterConversion)) {
        evaluated =
            convertConstantToType(arguments.front(), instruction.info.type);
      }
    }
    if (!evaluated && !voidResult) {
      return fail(frame, evaluated.failure == ConstantEvaluationFailure::None
                             ? ConstantEvaluationFailure::UnsupportedExpression
                             : evaluated.failure);
    }
    if (instruction.result) {
      frame.values[*instruction.result] = evaluated.value;
      ++frame.valueEpochs[*instruction.result];
      if (reference && frame.valueReferences.empty())
        frame.valueReferences.resize(frame.values.size());
      if (*instruction.result < frame.valueReferences.size())
        frame.valueReferences[*instruction.result] = reference;
    }
    if (instruction.loan && instruction.kind != MirInstructionKind::Borrow) {
      const auto *loan = frame.body.findLoan(*instruction.loan);
      if (!loan)
        return fail(frame, ConstantEvaluationFailure::InvalidOperands);
      if (!reference) {
        if (!instruction.result || !evaluated)
          return fail(frame, ConstantEvaluationFailure::UnsupportedExpression);
        reference = Location{.frame = &frame,
                             .root = MirPlaceRootKind::Value,
                             .cell = *instruction.result,
                             .epoch = frame.valueEpochs[*instruction.result],
                             .type = &observedType(instruction.info.type)};
      }
      if (loan->access == AccessMode::Mutable &&
          reference->access != AccessMode::Mutable)
        return fail(frame, ConstantEvaluationFailure::InvalidOperands);
      reference->access = loan->access;
      frame.loans[*instruction.loan] =
          Loan{std::move(*reference), !loan->twoPhase};
    }
    for (const auto &input : instruction.operands) {
      if (input.kind == MirOperandKind::Move) {
        const auto retired = retirePlace(frame, input.place);
        if (retired.failure != ConstantEvaluationFailure::None)
          return fail(frame, retired.failure);
      }
    }
    return {};
  }

  MirConstantEvaluation
  invoke(HirFunctionInstanceId id, std::span<const ConstantValue> arguments,
         std::size_t depth, std::optional<ConstantValue> receiver = {},
         std::span<const std::optional<Location>> argumentOrigins = {},
         std::optional<Location> receiverOrigin = {},
         std::optional<Location> *referenceResult = nullptr) {
    const auto *function = program.findFunctionInstance(id);
    if (function == nullptr || !function->constexprFunction ||
        function->requiresUnsafe || function->virtualMethod ||
        function->definitionKind != MirDefinitionKind::Source ||
        function->receiverMutability == ReceiverMutability::Consuming ||
        (function->returnBorrowOrigin != BorrowOriginKind::None &&
         function->returnType.kind != SemanticType::Reference) ||
        function->entryKind != ProgramEntryKind::None ||
        function->linkage != LanguageLinkage::Gti ||
        (!observedStorageType(function->returnType) &&
         function->returnType != SemanticType::Void) ||
        arguments.size() != function->parameterTypes.size()) {
      return {.failure = ConstantEvaluationFailure::UnsupportedExpression,
              .function = id,
              .detail =
                  "Constant MIR calls require a supported constexpr function."};
    }
    const bool instanceMethod = function->owner && !function->staticMember;
    if (instanceMethod != receiver.has_value())
      return {.failure = ConstantEvaluationFailure::UnsupportedExpression,
              .function = id};
    if (receiver) {
      const auto *owner = program.findClassInstance(*function->owner);
      if (!owner || !storageType(owner->type))
        return {.failure = ConstantEvaluationFailure::UnsupportedType,
                .function = id};
      const auto exact = exactValue(*receiver, owner->type);
      if (!exact)
        return {.failure = exact.failure, .function = id};
    }
    return executeBody(
        function->body, {MirBodyKind::Function, id}, function->returnType,
        function->parameterTypes, function->parameterBindings, arguments, depth,
        std::move(receiver), argumentOrigins, std::move(receiverOrigin),
        function->receiverMutability == ReceiverMutability::Mutable
            ? AccessMode::Mutable
            : AccessMode::ReadOnly,
        referenceResult);
  }

  MirConstantEvaluation
  construct(HirConstructorInstanceId id,
            std::span<const ConstantValue> arguments, std::size_t depth,
            std::span<const std::optional<Location>> origins = {}) {
    const auto *constructor = program.findConstructorInstance(id);
    const auto *owner =
        constructor ? program.findClassInstance(constructor->owner) : nullptr;
    if (!constructor || !owner ||
        (!constructor->constexprConstructor &&
         !constructor->generatedDefault) ||
        constructor->requiresUnsafe ||
        constructor->definitionKind != MirDefinitionKind::Source ||
        !storageType(owner->type) ||
        arguments.size() != constructor->parameterTypes.size())
      return {.failure = ConstantEvaluationFailure::UnsupportedExpression,
              .body = {MirBodyKind::Constructor, id}};
    // Every field must be initialized by this verified body. Declaration-owned
    // defaults require their own selected field-initializer execution schedule.
    if (constructor->initializers.size() != owner->declaredFields.size())
      return {.failure = ConstantEvaluationFailure::UnsupportedExpression,
              .body = {MirBodyKind::Constructor, id}};
    for (const auto &field : owner->declaredFields) {
      const auto count = std::count_if(
          constructor->initializers.begin(), constructor->initializers.end(),
          [&](const MirConstructorInitializer &initializer) {
            return initializer.kind ==
                       ConstructorInitializerTargetKind::Field &&
                   initializer.field == field.symbol &&
                   !initializer.generatedDefault &&
                   !initializer.storesReference &&
                   !initializer.constructorTarget;
          });
      if (count != 1)
        return {.failure = ConstantEvaluationFailure::UnsupportedExpression,
                .body = {MirBodyKind::Constructor, id}};
      // The declaration inventory is not an executable initialization. Some
      // existing constructor families still use a different MIR schedule;
      // refuse this profile before executing unless its exact field write is
      // represented. Never turn an unsupported schedule into a false
      // uninitialized-constant error after partially running the constructor.
      std::size_t writes = 0;
      for (const auto &block : constructor->body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != MirInstructionKind::Initialize ||
              instruction.constructorInitializer == 0 ||
              !instruction.destination)
            continue;
          const auto *place =
              constructor->body.findPlace(*instruction.destination);
          if (place && place->root == MirPlaceRootKind::This &&
              place->projections.size() == 1 &&
              place->projections[0].kind == MirProjectionKind::Field &&
              place->projections[0].field == field.symbol)
            ++writes;
        }
      }
      if (writes != 1)
        return {.failure = ConstantEvaluationFailure::UnsupportedExpression,
                .body = {MirBodyKind::Constructor, id}};
    }
    ConstantAggregate receiver{
        .kind = ConstantAggregateKind::Object,
        .identity = owner->declaration,
        .exactType = std::make_shared<const SemanticType>(owner->type)};
    return executeBody(constructor->body, {MirBodyKind::Constructor, id},
                       owner->type, constructor->parameterTypes,
                       constructor->parameterBindings, arguments, depth,
                       makeConstantAggregate(std::move(receiver)), origins, {},
                       AccessMode::Mutable);
  }

  MirConstantEvaluation
  executeBody(const MirBody &body, MirBodyAddress address,
              const SemanticType &returnType,
              const std::vector<SemanticType> &parameterTypes,
              const std::vector<HirBindingId> &parameterBindings,
              std::span<const ConstantValue> arguments, std::size_t depth,
              std::optional<ConstantValue> receiver = {},
              std::span<const std::optional<Location>> argumentOrigins = {},
              std::optional<Location> receiverOrigin = {},
              AccessMode receiverAccess = AccessMode::ReadOnly,
              std::optional<Location> *referenceResult = nullptr) {
    Frame frame{
        .body = body,
        .address = address,
        .values =
            std::vector<std::optional<ConstantValue>>(body.values.size() + 1),
        .places =
            std::vector<std::optional<ConstantValue>>(body.places.size() + 1),
        .loans = std::vector<std::optional<Loan>>(body.loans.size() + 1),
        .placeEpochs = std::vector<std::size_t>(body.places.size() + 1),
        .valueEpochs = std::vector<std::size_t>(body.values.size() + 1),
        .liveDrops = std::vector<bool>(body.dropObligations.size() + 1),
        .block = body.entry,
        .receiver = std::move(receiver),
        .receiverOrigin = std::move(receiverOrigin),
        .receiverAccess = receiverAccess};
    if (depth >= limits.callDepth) {
      return fail(frame, ConstantEvaluationFailure::ResourceLimit,
                  "The constexpr call depth exceeded the compiler limit.");
    }
    for (const auto &obligation : body.dropObligations) {
      frame.liveDrops[obligation.id] = obligation.initiallyActive;
      if (!storageType(obligation.dropType.type)) {
        return fail(frame, ConstantEvaluationFailure::UnsupportedType,
                    "Constant MIR cannot execute this cleanup type.");
      }
    }
    for (const auto &loan : body.loans) {
      const auto *source = body.findPlace(loan.source);
      const auto *dependency = body.findLoan(loan.storedDependency);
      // A constant text value owns its literal bytes in ConstantValue. A
      // checked reference formal still carries the ordinary MIR's separate
      // owner witness, but needs no transient allocation or stored-reference
      // object to execute. Keep both entry loans and their verified endpoints;
      // this does not admit nominal stored carriers or runtime text storage.
      const bool constantTextDependency =
          source && source->root == MirPlaceRootKind::Binding &&
          source->initiallyAvailable && source->projections.empty() &&
          source->type.kind == SemanticType::Reference &&
          source->type.referenceAccess == AccessMode::ReadOnly &&
          source->type.arguments.size() == 1 &&
          source->type.arguments.front().kind == SemanticType::StringView &&
          loan.access == AccessMode::ReadOnly && dependency &&
          dependency->entry && dependency->kind == MirLoanKind::Stored &&
          dependency->access == AccessMode::ReadOnly &&
          dependency->source == loan.source &&
          dependency->storedDependency == 0 &&
          dependency->designatedFields.empty() &&
          dependency->additionalDependencies.empty();
      if ((loan.storedDependency != 0 && !constantTextDependency) ||
          !loan.additionalSources().empty() || loan.lifecycleBound ||
          source == nullptr || !observedStorageType(source->type)) {
        return fail(frame, ConstantEvaluationFailure::UnsupportedType,
                    "Constant MIR only admits checked loans to supported "
                    "values, without stored reference carriers.");
      }
    }
    // Refuse optimizer-dependent eligibility, including rewrites in untaken
    // branches. Semantic staging must supply the canonical unoptimized body.
    for (const auto &block : body.blocks) {
      if (block.terminator.provenance.kind !=
              MirTerminatorProvenanceKind::None ||
          !block.parameters.empty()) {
        return fail(frame, ConstantEvaluationFailure::UnsupportedExpression,
                    "Constant evaluation requires canonical unoptimized MIR.");
      }
      for (const auto &instruction : block.instructions) {
        if (instruction.tombstone || (instruction.literalProvenance.kind !=
                                          MirLiteralProvenanceKind::None &&
                                      instruction.literalProvenance.kind !=
                                          MirLiteralProvenanceKind::Source)) {
          return fail(
              frame, ConstantEvaluationFailure::UnsupportedExpression,
              "Constant evaluation requires canonical unoptimized MIR.");
        }
      }
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const auto &type = parameterTypes[index];
      if (!observedStorageType(type)) {
        return fail(frame, ConstantEvaluationFailure::UnsupportedType);
      }
      const auto exact = exactValue(arguments[index], observedType(type));
      if (!exact) {
        return fail(
            frame, exact.failure,
            "Constant argument does not have the exact parameter type.");
      }
    }
    // Parameter types are concrete/expanded; bindings retain declaration
    // arity. Bind each concrete pack exactly once, including an empty pack.
    std::size_t argumentIndex = 0;
    for (const auto binding : parameterBindings) {
      const auto place = std::find_if(
          body.places.begin(), body.places.end(), [&](const auto &item) {
            return item.root == MirPlaceRootKind::Binding &&
                   item.projections.empty() && item.binding == binding;
          });
      const bool pack = place != body.places.end() &&
                        place->type.kind == SemanticType::TypePack;
      const auto count = pack ? place->type.arguments.size() : 1;
      if (count > arguments.size() - argumentIndex)
        return fail(frame, ConstantEvaluationFailure::InvalidOperands);
      if (place != body.places.end()) {
        const auto storage = cell(frame, place->id);
        if (!storage)
          return fail(frame, ConstantEvaluationFailure::UnsupportedType);
        if (pack) {
          if (!charge(count))
            return fail(frame, ConstantEvaluationFailure::ResourceLimit);
          ConstantAggregate value{
              .kind = ConstantAggregateKind::Array,
              .exactType = std::make_shared<const SemanticType>(place->type)};
          value.elements.assign(arguments.begin() + argumentIndex,
                                arguments.begin() + argumentIndex + count);
          frame.places[storage] = makeConstantAggregate(std::move(value));
        } else {
          frame.places[storage] = arguments[argumentIndex];
          if (place->type.kind == SemanticType::Reference &&
              argumentIndex < argumentOrigins.size() &&
              argumentOrigins[argumentIndex]) {
            if (frame.references.empty())
              frame.references.resize(frame.places.size());
            frame.references[storage] = argumentOrigins[argumentIndex];
          }
        }
        ++frame.placeEpochs[storage];
      }
      argumentIndex += count;
    }
    if (argumentIndex != arguments.size())
      return fail(frame, ConstantEvaluationFailure::InvalidOperands);
    for (const auto &loan : body.loans) {
      if (!loan.entry)
        continue;
      Location location;
      const auto located = locate(frame, loan.source, location);
      if (located.failure != ConstantEvaluationFailure::None)
        return fail(frame, located.failure);
      const auto value = readLocation(location);
      if (!value)
        return fail(frame, value.failure);
      if (loan.access == AccessMode::Mutable &&
          location.access != AccessMode::Mutable)
        return fail(
            frame, ConstantEvaluationFailure::InvalidOperands,
            "Entry loan requests mutable access to a read-only origin.");
      location.access = loan.access;
      frame.loans[loan.id] = Loan{std::move(location), true};
    }
    while (const auto *block = body.findBlock(frame.block)) {
      for (const auto &instruction : block->instructions) {
        frame.instruction = instruction.id;
        if (steps++ >= limits.steps) {
          return fail(frame, ConstantEvaluationFailure::ResourceLimit,
                      "Constant MIR execution-step limit exceeded.");
        }
        auto result = execute(frame, instruction, depth);
        if (result.failure != ConstantEvaluationFailure::None)
          return result;
        applyLifecycle(frame, instruction.lifecycle);
      }
      frame.instruction = 0;
      if (steps++ >= limits.steps) {
        return fail(frame, ConstantEvaluationFailure::ResourceLimit,
                    "Constant MIR execution-step limit exceeded.");
      }
      const auto &terminator = block->terminator;
      switch (terminator.kind) {
      case MirTerminatorKind::Goto:
      case MirTerminatorKind::Invoke:
        // A failed scalar operation already returned a located constant
        // failure. Successful Invoke follows exactly the represented edge.
        applyLifecycle(frame, terminator.successLifecycle);
        frame.block = terminator.target;
        break;
      case MirTerminatorKind::Branch: {
        const auto condition = terminator.value
                                   ? operand(frame, *terminator.value)
                                   : ConstantEvaluation{};
        const auto *value =
            condition ? std::get_if<bool>(&*condition.value) : nullptr;
        if (value == nullptr)
          return fail(frame, ConstantEvaluationFailure::InvalidOperands);
        frame.block = *value ? terminator.target : terminator.elseTarget;
        break;
      }
      case MirTerminatorKind::Switch: {
        const auto subject = terminator.value
                                 ? operand(frame, *terminator.value)
                                 : ConstantEvaluation{};
        if (!subject)
          return fail(frame, subject.failure);
        MirBlockId selected = terminator.target;
        for (const auto &arm : terminator.switchTargets) {
          if (!arm.value)
            return fail(frame, ConstantEvaluationFailure::InvalidOperands);
          const auto &label = *arm.value;
          ConstantValue value;
          if (label.kind == SwitchCaseKind::Character) {
            value = CharacterLiteral{
                static_cast<std::uint8_t>(label.value.magnitude)};
          } else if (label.kind == SwitchCaseKind::Integer) {
            const auto domain = constantIntegerDomain(label.type);
            if (!domain)
              return fail(frame, ConstantEvaluationFailure::UnsupportedType);
            value = makeConstantInteger({.negative = label.value.negative,
                                         .magnitude = label.value.magnitude},
                                        *domain);
          } else if (label.kind == SwitchCaseKind::Enumerator &&
                     label.type.kind == SemanticType::Enum &&
                     label.type.enumId == label.enumOwner) {
            const auto enumerator = enumValue(label.type, label.value);
            if (!enumerator)
              return fail(frame, enumerator.failure);
            value = *enumerator.value;
          } else {
            return fail(frame, ConstantEvaluationFailure::UnsupportedType);
          }
          const auto equal = evaluateConstantBinary(TokenKind::EQUAL_EQUAL,
                                                    *subject.value, value);
          if (!equal)
            return fail(frame, equal.failure);
          if (std::get<bool>(*equal.value)) {
            selected = arm.target;
            break;
          }
        }
        frame.block = selected;
        break;
      }
      case MirTerminatorKind::Return: {
        if (frame.address.kind == MirBodyKind::Constructor && frame.receiver) {
          const auto complete = exactValue(*frame.receiver, returnType);
          return complete ? MirConstantEvaluation{.value = complete.value}
                          : fail(frame, complete.failure);
        }
        if (returnType == SemanticType::Void)
          return {.voidResult = true};
        if (!terminator.value && returnType.kind == SemanticType::Expected &&
            returnType.arguments.size() == 2 &&
            returnType.arguments[0] == SemanticType::Void) {
          const auto success = makeConstantResult(returnType, true);
          return {.value = success.value, .failure = success.failure};
        }
        const auto result = terminator.value ? operand(frame, *terminator.value)
                                             : ConstantEvaluation{};
        if (!result)
          return fail(frame,
                      result.failure == ConstantEvaluationFailure::None
                          ? ConstantEvaluationFailure::NonConstantReference
                          : result.failure);
        if (returnType.kind == SemanticType::Reference && referenceResult) {
          Location location;
          const auto located =
              locateOperand(frame, *terminator.value, location);
          if (located.failure != ConstantEvaluationFailure::None)
            return fail(frame, located.failure);
          if (location.frame == &frame ||
              (returnType.referenceAccess == AccessMode::Mutable &&
               location.access != AccessMode::Mutable))
            return fail(frame, ConstantEvaluationFailure::NonConstantReference,
                        "A constant reference result must retain its caller's "
                        "live origin and access.");
          location.access = returnType.referenceAccess;
          *referenceResult = std::move(location);
        }
        const auto converted =
            convertValue(*result.value, observedType(returnType));
        return converted ? MirConstantEvaluation{.value = converted.value}
                         : fail(frame, converted.failure);
      }
      default:
        return fail(
            frame, ConstantEvaluationFailure::UnsupportedExpression,
            "Terminator is outside the constant MIR execution profile.");
      }
    }
    return fail(frame, ConstantEvaluationFailure::UnsupportedExpression);
  }

  const MirProgram &program;
  MirConstantLimits limits;
  std::size_t steps = 0;
  std::vector<std::unique_ptr<Allocation>> heap;
  std::size_t heapUnits = 0;
};
} // namespace

MirConstantEvaluation evaluateMirConstantFunction(
    const MirProgram &program, HirFunctionInstanceId function,
    std::span<const ConstantValue> arguments, MirConstantLimits limits,
    const std::optional<ConstantValue> &receiver) {
  return Evaluation(program, limits)
      .run({MirBodyKind::Function, function}, arguments, receiver);
}

MirConstantEvaluation evaluateMirConstantConstructor(
    const MirProgram &program, HirConstructorInstanceId constructor,
    std::span<const ConstantValue> arguments, MirConstantLimits limits) {
  return Evaluation(program, limits)
      .run({MirBodyKind::Constructor, constructor}, arguments, {});
}

} // namespace lang
