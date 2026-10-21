#pragma once

#include "gti/checked_integer.h"
#include "gti/token.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace lang {

struct SemanticType;

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

// Scoped enumerators retain their nominal owner.  The underlying integer is
// still available for backend materialization, but two unrelated enum types
// never become the same constant merely because their discriminants match.
struct ConstantEnum {
  std::size_t enumId = 0;
  ConstantInteger value;

  friend bool operator==(const ConstantEnum &, const ConstantEnum &) = default;
};

struct ConstantAggregate;

// Recursive constants use an immutable shared node so ConstantValue remains a
// regular, inexpensive value throughout semantics and the IR snapshots.
struct ConstantAggregateValue {
  std::shared_ptr<const ConstantAggregate> value;

  friend bool operator==(const ConstantAggregateValue &left,
                         const ConstantAggregateValue &right);
};

using ConstantValue =
    std::variant<ConstantInteger, BinaryFloat, CharacterLiteral, std::string,
                 bool, NullConstant, ConstantEnum, ConstantAggregateValue>;

enum class ConstantAggregateKind {
  Array,
  Object,
  // Executor-local symbolic ownership. Never accepted by constant conversion
  // or publication; only the MIR evaluator may create/resolve these nodes.
  // identity is a request-local allocation ID, not a host or target address.
  TransientOwner,
  PayloadEnum,
};

struct ConstantAggregateField {
  std::size_t symbol = 0;
  ConstantValue value;

  friend bool operator==(const ConstantAggregateField &,
                         const ConstantAggregateField &) = default;
};

struct ConstantAggregate {
  ConstantAggregateKind kind = ConstantAggregateKind::Array;
  // Zero for arrays; the exact semantic ClassId for structural objects.
  std::size_t identity = 0;
  // Retains the complete concrete type, including generic and value
  // arguments. The declaration identity alone is not sufficient for generic
  // aggregates.
  std::shared_ptr<const SemanticType> exactType;
  std::vector<ConstantValue> elements;
  std::vector<ConstantAggregateField> fields;
  // Exact active alternative for a payload enum; never an underlying integer.
  std::size_t variant = 0;

  friend bool operator==(const ConstantAggregate &, const ConstantAggregate &);
};

inline bool operator==(const ConstantAggregateValue &left,
                       const ConstantAggregateValue &right) {
  if (left.value == right.value) {
    return true;
  }
  return left.value != nullptr && right.value != nullptr &&
         *left.value == *right.value;
}

[[nodiscard]] inline ConstantAggregateValue
makeConstantAggregate(ConstantAggregate value) {
  return {std::make_shared<const ConstantAggregate>(std::move(value))};
}

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

[[nodiscard]] ConstantEvaluation
convertConstantToType(const ConstantValue &value, const SemanticType &target);

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
// Literal-backed text uses owned bytes in ConstantValue. These kernels are
// independent of AST spelling and can also serve staged MIR evaluation.
[[nodiscard]] ConstantEvaluation
evaluateConstantTextSize(const ConstantValue &text);
[[nodiscard]] ConstantEvaluation
evaluateConstantTextEmpty(const ConstantValue &text);
[[nodiscard]] ConstantEvaluation
evaluateConstantTextIndex(const ConstantValue &text,
                          const ConstantValue &index);
[[nodiscard]] ConstantEvaluation
convertConstantInteger(const ConstantValue &value, CheckedIntegerDomain target);
[[nodiscard]] ConstantEvaluation
convertConstantFloat(const ConstantValue &value,
                     BinaryFloatFormat format = BinaryFloatFormat::Binary32);
[[nodiscard]] std::optional<std::uint64_t>
constantUnsignedMagnitude(const ConstantValue &value);

} // namespace lang
