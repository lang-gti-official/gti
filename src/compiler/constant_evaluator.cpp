#include "gti/semantic_core.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>

namespace lang {

ConstantEvaluation constantResultHasValue(const ConstantValue &value) {
  if (const auto *node = std::get_if<ConstantAggregateValue>(&value);
      node && node->value && node->value->exactType &&
      node->value->exactType->kind == SemanticType::Expected &&
      (node->value->kind == ConstantAggregateKind::ExpectedSuccess ||
       node->value->kind == ConstantAggregateKind::ExpectedError) &&
      convertConstantToType(value, *node->value->exactType))
    return {.value =
                node->value->kind == ConstantAggregateKind::ExpectedSuccess};
  return {.failure = ConstantEvaluationFailure::InvalidOperands};
}

ConstantEvaluation constantResultPayload(const ConstantValue &value,
                                         bool success) {
  const auto state = constantResultHasValue(value);
  if (!state || std::get<bool>(*state.value) != success)
    return {.failure = ConstantEvaluationFailure::InvalidOperands};
  if (const auto *node = std::get_if<ConstantAggregateValue>(&value);
      node && node->value && node->value->elements.size() == 1)
    return {.value = node->value->elements.front()};
  return {.failure = ConstantEvaluationFailure::InvalidOperands};
}

ConstantEvaluation makeConstantResult(const SemanticType &type, bool success,
                                      std::optional<ConstantValue> payload) {
  const bool expected = type.kind == SemanticType::Expected;
  if ((expected && type.arguments.size() != 2) ||
      (!expected && (type.kind != SemanticType::Unexpected || success ||
                     type.arguments.size() != 1)))
    return {.failure = ConstantEvaluationFailure::UnsupportedType};
  if (type.arguments.back() == SemanticType::Void ||
      std::any_of(type.arguments.begin(), type.arguments.end(),
                  [](const auto &argument) {
                    return argument.kind == SemanticType::Reference;
                  }))
    return {.failure = ConstantEvaluationFailure::UnsupportedType};
  const auto &alternative = type.arguments[expected && !success ? 1 : 0];
  if (alternative.kind == SemanticType::Reference ||
      (alternative == SemanticType::Void) != !payload.has_value())
    return {.failure = ConstantEvaluationFailure::InvalidOperands};
  ConstantAggregate result{
      .kind = !expected ? ConstantAggregateKind::Unexpected
              : success ? ConstantAggregateKind::ExpectedSuccess
                        : ConstantAggregateKind::ExpectedError,
      .exactType = std::make_shared<const SemanticType>(type)};
  if (payload) {
    auto converted = convertConstantToType(*payload, alternative);
    if (!converted)
      return converted;
    result.elements.push_back(std::move(*converted.value));
  }
  return {.value = makeConstantAggregate(std::move(result))};
}

ConstantEvaluation convertConstantToType(const ConstantValue &value,
                                         const SemanticType &target) {
  if (const auto *enumerator = std::get_if<ConstantEnum>(&value)) {
    if (target.kind == SemanticType::Expected && target.arguments.size() == 2)
      return makeConstantResult(target, true, value);
    return target.kind == SemanticType::Enum && target.enumId != 0 &&
                   target.enumId == enumerator->enumId
               ? ConstantEvaluation{.value = value}
               : ConstantEvaluation{
                     .failure = ConstantEvaluationFailure::UnsupportedType};
  }
  if (const auto *aggregate = std::get_if<ConstantAggregateValue>(&value)) {
    if (target.kind == SemanticType::Expected && target.arguments.size() == 2 &&
        aggregate->value && aggregate->value->exactType &&
        aggregate->value->kind == ConstantAggregateKind::Unexpected &&
        aggregate->value->exactType->kind == SemanticType::Unexpected &&
        aggregate->value->exactType->arguments.size() == 1 &&
        aggregate->value->exactType->arguments[0] == target.arguments[1] &&
        aggregate->value->elements.size() == 1 &&
        convertConstantToType(value, *aggregate->value->exactType))
      return makeConstantResult(target, false, aggregate->value->elements[0]);
    if (target.kind == SemanticType::Expected && target.arguments.size() == 2 &&
        aggregate->value && aggregate->value->exactType &&
        *aggregate->value->exactType == target.arguments[0])
      return makeConstantResult(target, true, value);
    if (aggregate->value == nullptr || aggregate->value->exactType == nullptr ||
        *aggregate->value->exactType != target) {
      return {.failure = ConstantEvaluationFailure::UnsupportedType};
    }
    if (target.kind == SemanticType::Expected ||
        target.kind == SemanticType::Unexpected) {
      const auto &node = *aggregate->value;
      const bool success = node.kind == ConstantAggregateKind::ExpectedSuccess;
      const bool expected = target.kind == SemanticType::Expected;
      if (node.identity != 0 || !node.fields.empty() ||
          (expected ? target.arguments.size() != 2 ||
                          (!success &&
                           node.kind != ConstantAggregateKind::ExpectedError)
                    : target.arguments.size() != 1 ||
                          node.kind != ConstantAggregateKind::Unexpected))
        return {.failure = ConstantEvaluationFailure::InvalidOperands};
      const auto &alternative = target.arguments[expected && !success ? 1 : 0];
      if (target.arguments.back() == SemanticType::Void ||
          std::any_of(target.arguments.begin(), target.arguments.end(),
                      [](const auto &argument) {
                        return argument.kind == SemanticType::Reference;
                      }) ||
          node.elements.size() != (alternative == SemanticType::Void ? 0u : 1u))
        return {.failure = ConstantEvaluationFailure::InvalidOperands};
      // Like arrays/records, recursive exactness belongs to the budgeted
      // verifier/executor. Conversion preserves this immutable exact node.
      return {.value = value};
    }
    if (target.kind == SemanticType::Array &&
        aggregate->value->kind == ConstantAggregateKind::Array &&
        aggregate->value->elements.size() == target.arrayLength) {
      return {.value = value};
    }
    if (target.kind == SemanticType::Class && target.classId != 0 &&
        aggregate->value->kind == ConstantAggregateKind::Object &&
        aggregate->value->identity == target.classId) {
      return {.value = value};
    }
    return {.failure = ConstantEvaluationFailure::UnsupportedType};
  }
  if (target.kind == SemanticType::Expected && target.arguments.size() == 2)
    return makeConstantResult(target, true, value);
  if (target == SemanticType::UInt8) {
    if (const auto *character = std::get_if<CharacterLiteral>(&value)) {
      return {.value = ConstantValue{
                  makeConstantInteger({.magnitude = character->value},
                                      CheckedIntegerDomain{.width = 8})}};
    }
  }
  if (target == SemanticType::Char) {
    if (const auto *integer = std::get_if<ConstantInteger>(&value);
        integer != nullptr && !integer->negative &&
        integer->domain.width == 8 && !integer->domain.signedValue) {
      return {.value = ConstantValue{CharacterLiteral{
                  static_cast<std::uint8_t>(integer->magnitude)}}};
    }
  }
  if (const std::optional<CheckedIntegerDomain> domain =
          constantIntegerDomain(target)) {
    return convertConstantInteger(value, *domain);
  }
  if (const std::optional<BinaryFloatFormat> format =
          semanticFloatFormat(target)) {
    return convertConstantFloat(value, *format);
  }
  if ((target == SemanticType::Bool && std::holds_alternative<bool>(value)) ||
      (target == SemanticType::Char &&
       std::holds_alternative<CharacterLiteral>(value)) ||
      (target == SemanticType::StringView &&
       std::holds_alternative<std::string>(value)) ||
      (target == SemanticType::NullPtr &&
       std::holds_alternative<NullConstant>(value))) {
    return {.value = value};
  }
  return {.failure = ConstantEvaluationFailure::UnsupportedType};
}

ConstantEvaluation evaluateConstantTextSize(const ConstantValue &text) {
  const auto *bytes = std::get_if<std::string>(&text);
  if (bytes == nullptr) {
    return {.failure = ConstantEvaluationFailure::InvalidOperands};
  }
  return {.value = makeConstantInteger({.magnitude = bytes->size()},
                                       {.width = 64, .signedValue = false})};
}

ConstantEvaluation evaluateConstantTextEmpty(const ConstantValue &text) {
  const auto *bytes = std::get_if<std::string>(&text);
  if (bytes == nullptr) {
    return {.failure = ConstantEvaluationFailure::InvalidOperands};
  }
  return {.value = bytes->empty()};
}

ConstantEvaluation evaluateConstantTextIndex(const ConstantValue &text,
                                             const ConstantValue &index) {
  const auto *bytes = std::get_if<std::string>(&text);
  const auto offset = constantUnsignedMagnitude(index);
  if (bytes == nullptr || !offset || *offset >= bytes->size()) {
    return {.failure = ConstantEvaluationFailure::InvalidOperands};
  }
  // GTI char is an unsigned byte, independently of the host's char signedness.
  return {.value = CharacterLiteral{static_cast<std::uint8_t>(
              static_cast<unsigned char>((*bytes)[*offset]))}};
}

bool operator==(const ConstantAggregate &left, const ConstantAggregate &right) {
  const bool sameType =
      left.exactType == right.exactType ||
      (left.exactType != nullptr && right.exactType != nullptr &&
       *left.exactType == *right.exactType);
  return sameType && left.kind == right.kind &&
         left.identity == right.identity && left.elements == right.elements &&
         left.fields == right.fields;
}

[[nodiscard]] ConstantInteger makeConstantInteger(CheckedIntegerValue value,
                                                  CheckedIntegerDomain domain) {
  value = normalizeCheckedInteger(value);
  return {.negative = value.negative,
          .magnitude = value.magnitude,
          .domain = domain};
}

[[nodiscard]] CheckedIntegerValue
checkedIntegerValue(const ConstantInteger &value) {
  return {.negative = value.negative, .magnitude = value.magnitude};
}

[[nodiscard]] std::optional<CheckedIntegerDomain>
inferredIntegerDomain(std::uint64_t value) {
  if (value <=
      static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
    return CheckedIntegerDomain{.width = 32, .signedValue = true};
  }
  if (value <=
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return CheckedIntegerDomain{.width = 64, .signedValue = true};
  }
  return CheckedIntegerDomain{.width = 64};
}

[[nodiscard]] ConstantEvaluationFailure
constantFailure(CheckedIntegerFailure failure) {
  switch (failure) {
  case CheckedIntegerFailure::Overflow:
    return ConstantEvaluationFailure::IntegerOverflow;
  case CheckedIntegerFailure::DivisionByZero:
    return ConstantEvaluationFailure::DivisionByZero;
  case CheckedIntegerFailure::ModuloByZero:
    return ConstantEvaluationFailure::ModuloByZero;
  case CheckedIntegerFailure::NegativeShiftCount:
    return ConstantEvaluationFailure::NegativeShiftCount;
  case CheckedIntegerFailure::ShiftCountOutOfRange:
    return ConstantEvaluationFailure::ShiftCountOutOfRange;
  }
  return ConstantEvaluationFailure::InvalidOperands;
}

[[nodiscard]] ConstantEvaluation
constantFromChecked(const std::optional<CheckedIntegerOutcome> &outcome,
                    CheckedIntegerDomain domain) {
  if (!outcome) {
    return {.failure = ConstantEvaluationFailure::InvalidOperands};
  }
  if (const auto *value = std::get_if<CheckedIntegerValue>(&*outcome)) {
    return {.value = makeConstantInteger(*value, domain)};
  }
  return {.failure =
              constantFailure(std::get<CheckedIntegerFailure>(*outcome))};
}

[[nodiscard]] ConstantEvaluation
evaluateConstantLiteral(const Literal &literal,
                        std::optional<CheckedIntegerDomain> integerDomain) {
  if (const auto *integer = std::get_if<std::uint64_t>(&literal)) {
    const std::optional<CheckedIntegerDomain> domain =
        integerDomain ? integerDomain : inferredIntegerDomain(*integer);
    if (!domain || !checkedIntegerFits({.magnitude = *integer}, *domain)) {
      return {.failure = ConstantEvaluationFailure::IntegerOverflow};
    }
    return {.value = ConstantValue{
                makeConstantInteger({.magnitude = *integer}, *domain)}};
  }
  if (const auto *floating = std::get_if<BinaryFloat>(&literal)) {
    return {.value = ConstantValue{*floating}};
  }
  if (const auto *character = std::get_if<CharacterLiteral>(&literal)) {
    return {.value = ConstantValue{*character}};
  }
  if (const auto *string = std::get_if<std::string>(&literal)) {
    return {.value = ConstantValue{*string}};
  }
  if (const auto *boolean = std::get_if<bool>(&literal)) {
    return {.value = ConstantValue{*boolean}};
  }
  if (std::holds_alternative<std::nullptr_t>(literal)) {
    return {.value = ConstantValue{NullConstant{}}};
  }
  return {.failure = ConstantEvaluationFailure::UnsupportedExpression};
}

[[nodiscard]] std::optional<CheckedIntegerOperation>
constantIntegerOperation(TokenKind operation) {
  switch (operation) {
  case TokenKind::PLUS:
    return CheckedIntegerOperation::Add;
  case TokenKind::MINUS:
    return CheckedIntegerOperation::Subtract;
  case TokenKind::STAR:
    return CheckedIntegerOperation::Multiply;
  case TokenKind::SLASH:
    return CheckedIntegerOperation::Divide;
  case TokenKind::PERCENT:
    return CheckedIntegerOperation::Remainder;
  case TokenKind::AMPERSAND:
    return CheckedIntegerOperation::BitwiseAnd;
  case TokenKind::PIPE:
    return CheckedIntegerOperation::BitwiseOr;
  case TokenKind::CARET:
    return CheckedIntegerOperation::BitwiseXor;
  case TokenKind::SHIFT_LEFT:
    return CheckedIntegerOperation::ShiftLeft;
  case TokenKind::SHIFT_RIGHT:
    return CheckedIntegerOperation::ShiftRight;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] int compareConstantIntegers(const ConstantInteger &left,
                                          const ConstantInteger &right) {
  if (left.negative != right.negative) {
    return left.negative ? -1 : 1;
  }
  if (left.magnitude == right.magnitude) {
    return 0;
  }
  if (left.negative) {
    return left.magnitude > right.magnitude ? -1 : 1;
  }
  return left.magnitude < right.magnitude ? -1 : 1;
}

[[nodiscard]] std::optional<BinaryFloat>
constantBinaryFloat(const ConstantValue &value,
                    std::optional<BinaryFloatFormat> requestedFormat) {
  if (const auto *floating = std::get_if<BinaryFloat>(&value)) {
    return requestedFormat ? convertBinaryFloat(*floating, *requestedFormat)
                           : *floating;
  }
  if (const auto *integer = std::get_if<ConstantInteger>(&value)) {
    return integerToBinaryFloat(
        checkedIntegerValue(*integer), integer->domain,
        requestedFormat.value_or(BinaryFloatFormat::Binary32));
  }
  return std::nullopt;
}

[[nodiscard]] BinaryFloatFormat
constantBinaryFloatFormat(const ConstantValue &left,
                          const ConstantValue &right) {
  const auto format = [](const ConstantValue &value) {
    if (const auto *floating = std::get_if<BinaryFloat>(&value)) {
      return floating->format;
    }
    return BinaryFloatFormat::Binary32;
  };
  return format(left) == BinaryFloatFormat::Binary64 ||
                 format(right) == BinaryFloatFormat::Binary64
             ? BinaryFloatFormat::Binary64
             : BinaryFloatFormat::Binary32;
}

[[nodiscard]] ConstantEvaluation
evaluateConstantComparison(TokenKind operation, const ConstantValue &left,
                           const ConstantValue &right) {
  std::optional<int> ordering;
  if (const auto *leftInteger = std::get_if<ConstantInteger>(&left)) {
    if (const auto *rightInteger = std::get_if<ConstantInteger>(&right)) {
      ordering = compareConstantIntegers(*leftInteger, *rightInteger);
    }
  }
  if (!ordering) {
    const auto *leftEnum = std::get_if<ConstantEnum>(&left);
    const auto *rightEnum = std::get_if<ConstantEnum>(&right);
    if (leftEnum != nullptr && rightEnum != nullptr && leftEnum->enumId != 0 &&
        leftEnum->enumId == rightEnum->enumId) {
      ordering = compareConstantIntegers(leftEnum->value, rightEnum->value);
    }
  }
  if (!ordering && (std::holds_alternative<BinaryFloat>(left) ||
                    std::holds_alternative<BinaryFloat>(right))) {
    const BinaryFloatFormat format = constantBinaryFloatFormat(left, right);
    if (const std::optional<BinaryFloat> leftFloat =
            constantBinaryFloat(left, format);
        leftFloat) {
      const std::optional<BinaryFloat> rightFloat =
          constantBinaryFloat(right, format);
      if (!rightFloat) {
        return {.failure = ConstantEvaluationFailure::InvalidOperands};
      }
      const BinaryFloatOrdering floatOrdering =
          compareBinaryFloat(*leftFloat, *rightFloat);
      if (floatOrdering == BinaryFloatOrdering::Unordered) {
        switch (operation) {
        case TokenKind::EQUAL_EQUAL:
          return {.value = ConstantValue{false}};
        case TokenKind::BANG_EQUAL:
          return {.value = ConstantValue{true}};
        case TokenKind::LESS:
        case TokenKind::LESS_EQUAL:
        case TokenKind::GREATER:
        case TokenKind::GREATER_EQUAL:
          return {.value = ConstantValue{false}};
        default:
          return {.failure = ConstantEvaluationFailure::InvalidOperands};
        }
      }
      ordering = floatOrdering == BinaryFloatOrdering::Less      ? -1
                 : floatOrdering == BinaryFloatOrdering::Greater ? 1
                                                                 : 0;
    }
  } else if (const auto *leftString = std::get_if<std::string>(&left)) {
    if (const auto *rightString = std::get_if<std::string>(&right)) {
      ordering = leftString->compare(*rightString);
    }
  } else if (const auto *leftCharacter = std::get_if<CharacterLiteral>(&left)) {
    if (const auto *rightCharacter = std::get_if<CharacterLiteral>(&right)) {
      ordering = leftCharacter->value < rightCharacter->value   ? -1
                 : leftCharacter->value > rightCharacter->value ? 1
                                                                : 0;
    }
  } else if (const auto *leftBoolean = std::get_if<bool>(&left)) {
    if (const auto *rightBoolean = std::get_if<bool>(&right)) {
      ordering = *leftBoolean == *rightBoolean ? 0 : (*leftBoolean ? 1 : -1);
    }
  } else if (std::holds_alternative<NullConstant>(left) &&
             std::holds_alternative<NullConstant>(right)) {
    ordering = 0;
  }

  if (!ordering) {
    return {.failure = ConstantEvaluationFailure::InvalidOperands};
  }
  switch (operation) {
  case TokenKind::EQUAL_EQUAL:
    return {.value = ConstantValue{*ordering == 0}};
  case TokenKind::BANG_EQUAL:
    return {.value = ConstantValue{*ordering != 0}};
  case TokenKind::LESS:
    return {.value = ConstantValue{*ordering < 0}};
  case TokenKind::LESS_EQUAL:
    return {.value = ConstantValue{*ordering <= 0}};
  case TokenKind::GREATER:
    return {.value = ConstantValue{*ordering > 0}};
  case TokenKind::GREATER_EQUAL:
    return {.value = ConstantValue{*ordering >= 0}};
  default:
    return {.failure = ConstantEvaluationFailure::InvalidOperands};
  }
}

[[nodiscard]] ConstantEvaluation
evaluateConstantUnary(TokenKind operation, const ConstantValue &operand,
                      std::optional<CheckedIntegerDomain> resultDomain) {
  if (operation == TokenKind::BANG) {
    if (const auto *boolean = std::get_if<bool>(&operand)) {
      return {.value = ConstantValue{!*boolean}};
    }
    return {.failure = ConstantEvaluationFailure::InvalidOperands};
  }
  if (operation == TokenKind::PLUS) {
    if (const auto *integer = std::get_if<ConstantInteger>(&operand)) {
      const CheckedIntegerDomain domain =
          resultDomain.value_or(integer->domain);
      const CheckedIntegerValue value = checkedIntegerValue(*integer);
      if (!checkedIntegerFits(value, domain)) {
        return {.failure = ConstantEvaluationFailure::IntegerOverflow};
      }
      return {.value = ConstantValue{makeConstantInteger(value, domain)}};
    }
    if (const auto *floating = std::get_if<BinaryFloat>(&operand)) {
      return {.value = ConstantValue{*floating}};
    }
    return {.failure = ConstantEvaluationFailure::InvalidOperands};
  }
  if (operation == TokenKind::MINUS) {
    if (const auto *integer = std::get_if<ConstantInteger>(&operand)) {
      const CheckedIntegerDomain domain =
          resultDomain.value_or(integer->domain);
      return constantFromChecked(
          evaluateCheckedIntegerUnary(CheckedIntegerOperation::Negate,
                                      checkedIntegerValue(*integer), domain),
          domain);
    }
    if (const auto *floating = std::get_if<BinaryFloat>(&operand)) {
      return {.value = ConstantValue{negateBinaryFloat(*floating)}};
    }
    return {.failure = ConstantEvaluationFailure::InvalidOperands};
  }
  if (operation == TokenKind::TILDE) {
    if (const auto *integer = std::get_if<ConstantInteger>(&operand)) {
      const CheckedIntegerDomain domain =
          resultDomain.value_or(integer->domain);
      return constantFromChecked(
          evaluateCheckedIntegerUnary(CheckedIntegerOperation::BitwiseNot,
                                      checkedIntegerValue(*integer), domain),
          domain);
    }
  }
  return {.failure = ConstantEvaluationFailure::InvalidOperands};
}

[[nodiscard]] ConstantEvaluation
evaluateConstantBinary(TokenKind operation, const ConstantValue &left,
                       const ConstantValue &right,
                       std::optional<CheckedIntegerDomain> resultDomain,
                       std::optional<BinaryFloatFormat> resultFloatFormat) {
  if (std::holds_alternative<BinaryFloat>(left) ||
      std::holds_alternative<BinaryFloat>(right)) {
    const BinaryFloatFormat format =
        resultFloatFormat.value_or(constantBinaryFloatFormat(left, right));
    const std::optional<BinaryFloat> leftFloat =
        constantBinaryFloat(left, format);
    const std::optional<BinaryFloat> rightFloat =
        constantBinaryFloat(right, format);
    if (!leftFloat || !rightFloat) {
      return {.failure = ConstantEvaluationFailure::InvalidOperands};
    }
    std::optional<BinaryFloatOperation> floatOperation;
    switch (operation) {
    case TokenKind::PLUS:
      floatOperation = BinaryFloatOperation::Add;
      break;
    case TokenKind::MINUS:
      floatOperation = BinaryFloatOperation::Subtract;
      break;
    case TokenKind::STAR:
      floatOperation = BinaryFloatOperation::Multiply;
      break;
    case TokenKind::SLASH:
      floatOperation = BinaryFloatOperation::Divide;
      break;
    default:
      break;
    }
    if (floatOperation) {
      return {.value = ConstantValue{evaluateBinaryFloat(
                  *floatOperation, *leftFloat, *rightFloat)}};
    }
  }
  if (const auto integerOperation = constantIntegerOperation(operation)) {
    const auto *leftInteger = std::get_if<ConstantInteger>(&left);
    const auto *rightInteger = std::get_if<ConstantInteger>(&right);
    if (leftInteger == nullptr || rightInteger == nullptr) {
      return {.failure = ConstantEvaluationFailure::InvalidOperands};
    }
    if (!resultDomain && leftInteger->domain == rightInteger->domain) {
      resultDomain = leftInteger->domain;
    }
    if (!resultDomain) {
      return {.failure = ConstantEvaluationFailure::InvalidOperands};
    }
    return constantFromChecked(
        evaluateCheckedIntegerBinary(
            *integerOperation, checkedIntegerValue(*leftInteger),
            checkedIntegerValue(*rightInteger), *resultDomain),
        *resultDomain);
  }
  return evaluateConstantComparison(operation, left, right);
}

[[nodiscard]] ConstantEvaluation
evaluateConstantLogical(TokenKind operation, const ConstantValue &left,
                        const ConstantValue &right) {
  const auto *leftBoolean = std::get_if<bool>(&left);
  const auto *rightBoolean = std::get_if<bool>(&right);
  if (leftBoolean == nullptr || rightBoolean == nullptr) {
    return {.failure = ConstantEvaluationFailure::InvalidOperands};
  }
  if (operation == TokenKind::AND) {
    return {.value = ConstantValue{*leftBoolean && *rightBoolean}};
  }
  if (operation == TokenKind::OR) {
    return {.value = ConstantValue{*leftBoolean || *rightBoolean}};
  }
  return {.failure = ConstantEvaluationFailure::InvalidOperands};
}

[[nodiscard]] ConstantEvaluation
convertConstantInteger(const ConstantValue &value,
                       CheckedIntegerDomain target) {
  if (const auto *integer = std::get_if<ConstantInteger>(&value)) {
    const CheckedIntegerValue converted = checkedIntegerValue(*integer);
    if (!checkedIntegerFits(converted, target)) {
      return {.failure = ConstantEvaluationFailure::ConversionOutOfRange};
    }
    return {.value = ConstantValue{makeConstantInteger(converted, target)}};
  }
  if (const auto *floating = std::get_if<BinaryFloat>(&value)) {
    const std::optional<CheckedIntegerValue> converted =
        binaryFloatToInteger(*floating, target);
    if (!converted) {
      return {.failure = ConstantEvaluationFailure::ConversionOutOfRange};
    }
    return {.value = ConstantValue{makeConstantInteger(*converted, target)}};
  }
  return {.failure = ConstantEvaluationFailure::InvalidOperands};
}

[[nodiscard]] ConstantEvaluation
convertConstantFloat(const ConstantValue &value, BinaryFloatFormat format) {
  if (const auto *floating = std::get_if<BinaryFloat>(&value)) {
    return {.value = ConstantValue{convertBinaryFloat(*floating, format)}};
  }
  if (const auto *integer = std::get_if<ConstantInteger>(&value)) {
    const std::optional<BinaryFloat> converted = integerToBinaryFloat(
        checkedIntegerValue(*integer), integer->domain, format);
    if (converted) {
      return {.value = ConstantValue{*converted}};
    }
    return {.failure = ConstantEvaluationFailure::ConversionOutOfRange};
  }
  return {.failure = ConstantEvaluationFailure::InvalidOperands};
}

[[nodiscard]] std::optional<std::uint64_t>
constantUnsignedMagnitude(const ConstantValue &value) {
  const auto *integer = std::get_if<ConstantInteger>(&value);
  if (integer == nullptr || integer->negative) {
    return std::nullopt;
  }
  return integer->magnitude;
}

} // namespace lang
