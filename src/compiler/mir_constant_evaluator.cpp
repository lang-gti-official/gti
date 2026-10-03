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
    auto result = root.kind == MirBodyKind::Constructor
                      ? construct(root.owner, arguments, 0)
                      : invoke(root.owner, arguments, 0, receiver);
    result.steps = steps;
    return result;
  }

private:
  const SemanticType &observedType(const SemanticType &type) const {
    return type.kind == SemanticType::Reference &&
                   type.referenceAccess == AccessMode::ReadOnly &&
                   type.arguments.size() == 1
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
    if (depth >= limits.callDepth)
      return false;
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
        instance->requiresActiveCleanup || instance->destructor ||
        instance->lifecycleBoundBorrowCarrier || !instance->traits.copyable ||
        instance->traits.containsBorrowedState)
      return false;
    return std::all_of(instance->declaredFields.begin(),
                       instance->declaredFields.end(),
                       [&](const MirClassFieldInfo &field) {
                         return !field.requiresActiveCleanup &&
                                storageType(field.type, depth + 1);
                       });
  }

  struct Frame {
    const MirBody &body;
    MirBodyAddress address;
    std::vector<std::optional<ConstantValue>> values;
    std::vector<std::optional<ConstantValue>> places;
    std::vector<std::optional<ConstantValue>> loans;
    MirBlockId block = 0;
    MirInstructionId instruction = 0;
    std::optional<ConstantValue> receiver;
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

  bool charge(std::size_t amount) {
    if (amount > limits.steps - std::min(steps, limits.steps))
      return false;
    steps += amount;
    return true;
  }

  ConstantEvaluation exactValue(const ConstantValue &value,
                                const SemanticType &type) {
    if (!charge(1))
      return {.failure = ConstantEvaluationFailure::ResourceLimit};
    const auto converted = convertConstantToType(value, type);
    if (!converted || *converted.value != value)
      return {.failure = ConstantEvaluationFailure::InvalidOperands};
    const auto *aggregate = std::get_if<ConstantAggregateValue>(&value);
    if (!aggregate)
      return converted;
    if (type.kind == SemanticType::Array) {
      for (const auto &element : aggregate->value->elements) {
        const auto valid = exactValue(element, type.arguments[0]);
        if (!valid)
          return valid;
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

  ConstantEvaluation project(Frame &frame, const ConstantValue &value,
                             const MirPlace &place, std::size_t offset,
                             const ConstantValue *replacement = nullptr) {
    if (offset == place.projections.size()) {
      return replacement ? convertConstantToType(*replacement, place.type)
                         : ConstantEvaluation{.value = value};
    }
    if (!charge(1))
      return {.failure = ConstantEvaluationFailure::ResourceLimit};
    const auto &projection = place.projections[offset];
    if (projection.kind == MirProjectionKind::Dereference && offset == 0 &&
        place.root == MirPlaceRootKind::Binding) {
      const auto root = cell(frame, place.id);
      const auto *binding = frame.body.findPlace(root);
      if (binding && binding->type.kind == SemanticType::Reference &&
          observedStorageType(binding->type))
        return replacement
                   ? ConstantEvaluation{.failure = ConstantEvaluationFailure::
                                            UnsupportedExpression}
                   : project(frame, value, place, offset + 1);
    }
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
      auto result =
          project(frame, field->value, place, offset + 1, replacement);
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
    auto index = projection.constantIndex;
    if (projection.index != 0) {
      if (projection.index >= frame.values.size() ||
          !frame.values[projection.index])
        return {.failure = ConstantEvaluationFailure::NonConstantReference};
      index = constantUnsignedMagnitude(*frame.values[projection.index]);
    }
    const auto *aggregate = std::get_if<ConstantAggregateValue>(&value);
    if (!aggregate || !aggregate->value ||
        aggregate->value->kind != ConstantAggregateKind::Array)
      return {.failure = ConstantEvaluationFailure::UnsupportedType};
    if (!index || *index >= aggregate->value->elements.size())
      return {.failure = ConstantEvaluationFailure::InvalidOperands};
    auto result = project(frame, aggregate->value->elements[*index], place,
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

  ConstantEvaluation readPlace(Frame &frame, MirPlaceId id) {
    const auto *place = frame.body.findPlace(id);
    if (place && place->root == MirPlaceRootKind::Loan &&
        place->loan < frame.loans.size() && frame.loans[place->loan])
      return project(frame, *frame.loans[place->loan], *place, 0);
    if (place && place->root == MirPlaceRootKind::This && frame.receiver)
      return project(frame, *frame.receiver, *place, 0);
    // Materialized call/constructor results are ordinary Value-root places,
    // not binding storage. Their SSA value remains live until the represented
    // full-expression drop or ownership transfer retires it.
    if (place && place->root == MirPlaceRootKind::Value &&
        observedStorageType(place->type)) {
      if (place->value == 0 || place->value >= frame.values.size() ||
          !frame.values[place->value])
        return {.failure = ConstantEvaluationFailure::NonConstantReference};
      return project(frame, *frame.values[place->value], *place, 0);
    }
    const auto storage = cell(frame, id);
    if (!storage)
      return {.failure = ConstantEvaluationFailure::UnsupportedExpression};
    if (!frame.places[storage])
      return {.failure = ConstantEvaluationFailure::NonConstantReference};
    return project(frame, *frame.places[storage], *place, 0);
  }

  bool retirePlace(Frame &frame, MirPlaceId id) {
    const auto *place = frame.body.findPlace(id);
    if (!place || !place->projections.empty() || !storageType(place->type))
      return false;
    if (place->root == MirPlaceRootKind::Value && place->value != 0 &&
        place->value < frame.values.size()) {
      frame.values[place->value].reset();
      return true;
    }
    const auto storage = cell(frame, id);
    if (!storage)
      return false;
    frame.places[storage].reset();
    return true;
  }

  ConstantEvaluation writePlace(Frame &frame, MirPlaceId id,
                                const ConstantValue &value,
                                bool initializing = false) {
    const auto *target = frame.body.findPlace(id);
    if (target && target->root == MirPlaceRootKind::This && frame.receiver) {
      if (frame.address.kind != MirBodyKind::Constructor || !initializing ||
          target->projections.size() != 1 ||
          target->projections[0].kind != MirProjectionKind::Field)
        return {.failure = ConstantEvaluationFailure::UnsupportedExpression};
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
    const auto storage = cell(frame, id);
    if (!storage)
      return {.failure = ConstantEvaluationFailure::UnsupportedExpression};
    const auto &place = *target;
    const auto *root = frame.body.findPlace(storage);
    if (root->type.kind == SemanticType::Reference &&
        (!initializing || !place.projections.empty()))
      return {.failure = ConstantEvaluationFailure::UnsupportedExpression};
    if (!place.projections.empty() && !frame.places[storage])
      return {.failure = ConstantEvaluationFailure::NonConstantReference};
    auto updated =
        place.projections.empty()
            ? convertConstantToType(value, observedType(place.type))
            : project(frame, *frame.places[storage], place, 0, &value);
    if (updated)
      frame.places[storage] = std::move(updated.value);
    return updated.failure == ConstantEvaluationFailure::None
               ? ConstantEvaluation{.value = value}
               : updated;
  }

  ConstantEvaluation defaultValue(const SemanticType &type) {
    if (!charge(1))
      return {.failure = ConstantEvaluationFailure::ResourceLimit};
    if (type.kind == SemanticType::Array)
      return arrayValue(type, {});
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
    return {.failure = ConstantEvaluationFailure::UnsupportedType};
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
      return input.place ? readPlace(frame, input.place)
                         : ConstantEvaluation{.value = frame.loans[input.loan]};
    }
    if (input.kind == MirOperandKind::Constant && input.literal) {
      return evaluateConstantLiteral(*input.literal,
                                     constantIntegerDomain(input.type));
    }
    if (input.kind == MirOperandKind::Value && input.value != 0 &&
        input.value < frame.values.size() && frame.values[input.value]) {
      return {.value = frame.values[input.value]};
    }
    if (input.kind == MirOperandKind::Copy ||
        input.kind == MirOperandKind::Move ||
        input.kind == MirOperandKind::BorrowRead) {
      return readPlace(frame, input.place);
    }
    return {.failure = ConstantEvaluationFailure::NonConstantReference};
  }

  ConstantEvaluation compute(const MirInstruction &instruction,
                             const std::vector<ConstantValue> &values) {
    const auto &type = instruction.info.type;
    if (instruction.operation == MirOperation::Aggregate &&
        type.kind == SemanticType::Array)
      return arrayValue(type, values);
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

  MirConstantEvaluation execute(Frame &frame, const MirInstruction &instruction,
                                std::size_t depth) {
    // Loans hold immutable observations, never host addresses. The verified
    // source loan prevents writes/movement during the observation's lifetime.
    if (instruction.kind == MirInstructionKind::EndBorrow && instruction.loan) {
      frame.loans[*instruction.loan].reset();
      return {};
    }
    if (instruction.rawMemoryAccess ||
        instruction.unsafeOperation != UnsafeOperationKind::None ||
        instruction.bodyTarget || instruction.lambdaTarget ||
        (instruction.callableInvocation &&
         (*instruction.callableInvocation !=
              CallableInvocationCapability::Read ||
          instruction.kind != MirInstructionKind::Call ||
          !instruction.functionTarget ||
          instruction.dispatch != CallDispatch::Static))) {
      return fail(frame, ConstantEvaluationFailure::UnsupportedExpression,
                  "Operation is outside the scalar constant MIR profile.");
    }
    if (instruction.kind == MirInstructionKind::Lifecycle) {
      return {};
    }
    if (instruction.kind == MirInstructionKind::Drop) {
      // All admitted obligations have passive structural types. No user
      // destructor or allocation is hidden by retiring their local storage.
      if (instruction.destination) {
        if (!retirePlace(frame, *instruction.destination))
          return fail(frame, ConstantEvaluationFailure::UnsupportedExpression);
      }
      return {};
    }
    if (!observedStorageType(instruction.info.type)) {
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
    switch (instruction.kind) {
    case MirInstructionKind::Borrow:
      if (instruction.loan && arguments.size() == 1) {
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
        evaluated.value = arguments.front();
      }
      break;
    case MirInstructionKind::Initialize:
    case MirInstructionKind::Assign:
      if (arguments.size() == 1 && instruction.destination) {
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
          evaluated = convertConstantToType(
              arguments.front(), observedType(instruction.info.type));
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
    case MirInstructionKind::Construct: {
      if (!instruction.constructorTarget || instruction.objectAllocation ||
          instruction.receiver)
        return fail(frame, ConstantEvaluationFailure::UnsupportedExpression);
      auto result =
          construct(*instruction.constructorTarget, arguments, depth + 1);
      if (!result)
        return result;
      evaluated.value = std::move(result.value);
      break;
    }
    case MirInstructionKind::Call:
      if (instruction.intrinsic == IntrinsicKind::ArraySize &&
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
        std::optional<ConstantValue> receiver;
        if (instruction.receiver) {
          auto value = operand(frame, *instruction.receiver);
          if (!value)
            return fail(frame, value.failure);
          receiver = std::move(value.value);
        }
        auto result = invoke(*instruction.functionTarget, arguments, depth + 1,
                             std::move(receiver));
        if (!result)
          return result;
        evaluated.value = std::move(result.value);
      } else if (instruction.receiver && arguments.empty() &&
                 (instruction.intrinsic == IntrinsicKind::StringViewSize ||
                  instruction.intrinsic == IntrinsicKind::StringViewEmpty)) {
        const auto text = operand(frame, *instruction.receiver);
        if (!text)
          return fail(frame, text.failure);
        evaluated = instruction.intrinsic == IntrinsicKind::StringViewSize
                        ? evaluateConstantTextSize(*text.value)
                        : evaluateConstantTextEmpty(*text.value);
      } else if (!instruction.receiver && arguments.size() == 1 &&
                 (instruction.intrinsic ==
                      IntrinsicKind::NumericAliasConversion ||
                  instruction.intrinsic ==
                      IntrinsicKind::NumericTypeParameterConversion)) {
        evaluated =
            convertConstantToType(arguments.front(), instruction.info.type);
      }
      break;
    default:
      return fail(frame, ConstantEvaluationFailure::UnsupportedExpression,
                  "Instruction is outside the scalar constant MIR profile.");
    }
    if (!evaluated) {
      return fail(frame, evaluated.failure == ConstantEvaluationFailure::None
                             ? ConstantEvaluationFailure::UnsupportedExpression
                             : evaluated.failure);
    }
    if (instruction.result)
      frame.values[*instruction.result] = evaluated.value;
    if (instruction.loan)
      frame.loans[*instruction.loan] = evaluated.value;
    for (const auto &input : instruction.operands) {
      if (input.kind == MirOperandKind::Move) {
        if (!retirePlace(frame, input.place))
          return fail(frame, ConstantEvaluationFailure::UnsupportedExpression);
      }
    }
    return {};
  }

  MirConstantEvaluation invoke(HirFunctionInstanceId id,
                               std::span<const ConstantValue> arguments,
                               std::size_t depth,
                               std::optional<ConstantValue> receiver = {}) {
    const auto *function = program.findFunctionInstance(id);
    if (function == nullptr || !function->constexprFunction ||
        function->requiresUnsafe || function->virtualMethod ||
        function->definitionKind != MirDefinitionKind::Source ||
        function->receiverMutability != ReceiverMutability::ReadOnly ||
        (function->returnBorrowOrigin != BorrowOriginKind::None &&
         (function->returnType.kind != SemanticType::Reference ||
          function->returnType.referenceAccess != AccessMode::ReadOnly)) ||
        function->entryKind != ProgramEntryKind::None ||
        function->linkage != LanguageLinkage::Gti ||
        !observedStorageType(function->returnType) ||
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
    return executeBody(function->body, {MirBodyKind::Function, id},
                       function->returnType, function->parameterTypes,
                       function->parameterBindings, arguments, depth,
                       std::move(receiver));
  }

  MirConstantEvaluation construct(HirConstructorInstanceId id,
                                  std::span<const ConstantValue> arguments,
                                  std::size_t depth) {
    const auto *constructor = program.findConstructorInstance(id);
    const auto *owner =
        constructor ? program.findClassInstance(constructor->owner) : nullptr;
    if (!constructor || !owner || !constructor->constexprConstructor ||
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
                       makeConstantAggregate(std::move(receiver)));
  }

  MirConstantEvaluation
  executeBody(const MirBody &body, MirBodyAddress address,
              const SemanticType &returnType,
              const std::vector<SemanticType> &parameterTypes,
              const std::vector<HirBindingId> &parameterBindings,
              std::span<const ConstantValue> arguments, std::size_t depth,
              std::optional<ConstantValue> receiver = {}) {
    Frame frame{
        body,
        address,
        std::vector<std::optional<ConstantValue>>(body.values.size() + 1),
        std::vector<std::optional<ConstantValue>>(body.places.size() + 1),
        std::vector<std::optional<ConstantValue>>(body.loans.size() + 1),
        body.entry,
        0,
        std::move(receiver)};
    if (depth >= limits.callDepth) {
      return fail(frame, ConstantEvaluationFailure::ResourceLimit,
                  "The constexpr call depth exceeded the compiler limit.");
    }
    for (const auto &obligation : body.dropObligations) {
      if (obligation.dropType.requiresActiveCleanup ||
          !storageType(obligation.dropType.type)) {
        return fail(frame, ConstantEvaluationFailure::UnsupportedType,
                    "Constant MIR excludes active cleanup.");
      }
    }
    for (const auto &loan : body.loans) {
      const auto *source = body.findPlace(loan.source);
      if (loan.access != AccessMode::ReadOnly || loan.storedDependency != 0 ||
          !loan.additionalSources.empty() || loan.lifecycleBound ||
          loan.twoPhase || source == nullptr ||
          !observedStorageType(source->type)) {
        return fail(frame, ConstantEvaluationFailure::UnsupportedType,
                    "Constant MIR only admits read-only loans to supported "
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
      for (const auto &place : body.places) {
        if (place.root == MirPlaceRootKind::Binding &&
            place.projections.empty() &&
            place.binding == parameterBindings[index]) {
          const auto storage = cell(frame, place.id);
          if (storage != 0)
            frame.places[storage] = arguments[index];
        }
      }
    }
    for (const auto &loan : body.loans) {
      if (!loan.entry)
        continue;
      const auto value = readPlace(frame, loan.source);
      if (!value)
        return fail(frame, value.failure);
      frame.loans[loan.id] = value.value;
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
        const auto result = terminator.value ? operand(frame, *terminator.value)
                                             : ConstantEvaluation{};
        if (!result)
          return fail(frame, ConstantEvaluationFailure::NonConstantReference);
        const auto converted =
            convertConstantToType(*result.value, observedType(returnType));
        return converted ? MirConstantEvaluation{.value = converted.value}
                         : fail(frame, converted.failure);
      }
      default:
        return fail(frame, ConstantEvaluationFailure::UnsupportedExpression,
                    "Terminator is outside the scalar constant MIR profile.");
      }
    }
    return fail(frame, ConstantEvaluationFailure::UnsupportedExpression);
  }

  const MirProgram &program;
  MirConstantLimits limits;
  std::size_t steps = 0;
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
