#pragma once

#include "gti/checked_integer.h"
#include "gti/token.h"

#include <cstdint>
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

// The bounded constexpr representation for the public checked-integer result
// family. An empty value denotes arithmetic_errc::result_out_of_range. The
// source-level error type remains part of SemanticType rather than this
// language-neutral value record.
struct ConstantCheckedIntegerResult {
  CheckedIntegerDomain domain;
  std::optional<ConstantInteger> value;

  friend bool operator==(const ConstantCheckedIntegerResult &,
                         const ConstantCheckedIntegerResult &) = default;
};

using ConstantValue =
    std::variant<ConstantInteger, BinaryFloat, CharacterLiteral, std::string,
                 bool, NullConstant, ConstantCheckedIntegerResult>;

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

[[nodiscard]] ConstantInteger makeConstantInteger(CheckedIntegerValue value,
                                                  CheckedIntegerDomain domain);
[[nodiscard]] CheckedIntegerValue
checkedIntegerValue(const ConstantInteger &value);
[[nodiscard]] std::optional<CheckedIntegerDomain>
inferredIntegerDomain(std::uint64_t value);
[[nodiscard]] ConstantEvaluationFailure
constantFailure(CheckedIntegerFailure failure);
[[nodiscard]] ConstantEvaluation
constantFromChecked(const std::optional<CheckedIntegerOutcome> &outcome,
                    CheckedIntegerDomain domain);
[[nodiscard]] ConstantEvaluation evaluateConstantLiteral(
    const Literal &literal,
    std::optional<CheckedIntegerDomain> integerDomain = std::nullopt);
[[nodiscard]] std::optional<CheckedIntegerOperation>
constantIntegerOperation(TokenKind operation);
[[nodiscard]] int compareConstantIntegers(const ConstantInteger &left,
                                          const ConstantInteger &right);
[[nodiscard]] std::optional<BinaryFloat> constantBinaryFloat(
    const ConstantValue &value,
    std::optional<BinaryFloatFormat> requestedFormat = std::nullopt);
[[nodiscard]] BinaryFloatFormat
constantBinaryFloatFormat(const ConstantValue &left,
                          const ConstantValue &right);
[[nodiscard]] ConstantEvaluation
evaluateConstantComparison(TokenKind operation, const ConstantValue &left,
                           const ConstantValue &right);
[[nodiscard]] ConstantEvaluation evaluateConstantUnary(
    TokenKind operation, const ConstantValue &operand,
    std::optional<CheckedIntegerDomain> resultDomain = std::nullopt);
[[nodiscard]] ConstantEvaluation evaluateConstantBinary(
    TokenKind operation, const ConstantValue &left, const ConstantValue &right,
    std::optional<CheckedIntegerDomain> resultDomain = std::nullopt,
    std::optional<BinaryFloatFormat> resultFloatFormat = std::nullopt);
[[nodiscard]] ConstantEvaluation
evaluateConstantLogical(TokenKind operation, const ConstantValue &left,
                        const ConstantValue &right);
[[nodiscard]] ConstantEvaluation
convertConstantInteger(const ConstantValue &value, CheckedIntegerDomain target);
[[nodiscard]] ConstantEvaluation
convertConstantFloat(const ConstantValue &value,
                     BinaryFloatFormat format = BinaryFloatFormat::Binary32);
[[nodiscard]] std::optional<std::uint64_t>
constantUnsignedMagnitude(const ConstantValue &value);

} // namespace lang
