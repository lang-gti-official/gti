#pragma once

#include "gti/checked_integer.h"
#include "gti/token.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>

namespace lang {

struct ConstantInteger {
  bool negative = false;
  std::uint64_t magnitude = 0;
  CheckedIntegerDomain domain;

  friend bool operator==(const ConstantInteger &,
                         const ConstantInteger &) = default;
};

struct NullConstant {
  friend bool operator==(NullConstant, NullConstant) = default;
};

using ConstantValue =
    std::variant<ConstantInteger, BinaryFloat, CharacterLiteral, std::string,
                 bool, NullConstant>;

enum class ConstantEvaluationFailure {
  None,
  UnsupportedExpression,
  UnsupportedType,
  NonConstantReference,
  ResourceLimit,
  InvalidOperands,
  IntegerOverflow,
  DivisionByZero,
  ModuloByZero,
  NegativeShiftCount,
  ShiftCountOutOfRange,
  ConversionOutOfRange,
};

struct ConstantEvaluation {
  std::optional<ConstantValue> value;
  ConstantEvaluationFailure failure = ConstantEvaluationFailure::None;

  [[nodiscard]] explicit operator bool() const { return value.has_value(); }
};

[[nodiscard]] inline ConstantInteger
makeConstantInteger(CheckedIntegerValue value, CheckedIntegerDomain domain) {
  value = normalizeCheckedInteger(value);
  return {.negative = value.negative,
          .magnitude = value.magnitude,
          .domain = domain};
}

[[nodiscard]] inline CheckedIntegerValue
checkedIntegerValue(const ConstantInteger &value) {
  return {.negative = value.negative, .magnitude = value.magnitude};
}

[[nodiscard]] inline std::optional<CheckedIntegerDomain>
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

[[nodiscard]] inline ConstantEvaluationFailure
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

[[nodiscard]] inline ConstantEvaluation
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

[[nodiscard]] inline ConstantEvaluation evaluateConstantLiteral(
    const Literal &literal,
    std::optional<CheckedIntegerDomain> integerDomain = std::nullopt) {
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

[[nodiscard]] inline std::optional<CheckedIntegerOperation>
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

[[nodiscard]] inline int compareConstantIntegers(const ConstantInteger &left,
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

[[nodiscard]] inline std::optional<BinaryFloat> constantBinaryFloat(
    const ConstantValue &value,
    std::optional<BinaryFloatFormat> requestedFormat = std::nullopt) {
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

[[nodiscard]] inline BinaryFloatFormat
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

[[nodiscard]] inline ConstantEvaluation
evaluateConstantComparison(TokenKind operation, const ConstantValue &left,
                           const ConstantValue &right) {
  std::optional<int> ordering;
  if (const auto *leftInteger = std::get_if<ConstantInteger>(&left)) {
    if (const auto *rightInteger = std::get_if<ConstantInteger>(&right)) {
      ordering = compareConstantIntegers(*leftInteger, *rightInteger);
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

[[nodiscard]] inline ConstantEvaluation evaluateConstantUnary(
    TokenKind operation, const ConstantValue &operand,
    std::optional<CheckedIntegerDomain> resultDomain = std::nullopt) {
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

[[nodiscard]] inline ConstantEvaluation evaluateConstantBinary(
    TokenKind operation, const ConstantValue &left, const ConstantValue &right,
    std::optional<CheckedIntegerDomain> resultDomain = std::nullopt,
    std::optional<BinaryFloatFormat> resultFloatFormat = std::nullopt) {
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
    if (leftInteger == nullptr || rightInteger == nullptr || !resultDomain) {
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

[[nodiscard]] inline ConstantEvaluation
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

[[nodiscard]] inline ConstantEvaluation
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

[[nodiscard]] inline ConstantEvaluation
convertConstantFloat(const ConstantValue &value,
                     BinaryFloatFormat format = BinaryFloatFormat::Binary32) {
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

[[nodiscard]] inline std::optional<std::uint64_t>
constantUnsignedMagnitude(const ConstantValue &value) {
  const auto *integer = std::get_if<ConstantInteger>(&value);
  if (integer == nullptr || integer->negative) {
    return std::nullopt;
  }
  return integer->magnitude;
}

} // namespace lang
