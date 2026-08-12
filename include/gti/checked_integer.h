#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <variant>

namespace lang {

struct CheckedIntegerValue {
  bool negative = false;
  std::uint64_t magnitude = 0;

  friend bool operator==(const CheckedIntegerValue &,
                         const CheckedIntegerValue &) = default;
};

struct CheckedIntegerDomain {
  std::uint8_t width = 0;
  bool signedValue = false;

  friend bool operator==(const CheckedIntegerDomain &,
                         const CheckedIntegerDomain &) = default;
};

enum class CheckedIntegerOperation {
  Add,
  Subtract,
  Multiply,
  Divide,
  Remainder,
  BitwiseAnd,
  BitwiseOr,
  BitwiseXor,
  ShiftLeft,
  ShiftRight,
  Negate,
  BitwiseNot,
};

enum class CheckedIntegerFailure {
  Overflow,
  DivisionByZero,
  ModuloByZero,
  NegativeShiftCount,
  ShiftCountOutOfRange,
};

enum class IntegerArithmeticMode {
  Wrapping,
  Saturating,
};

using CheckedIntegerOutcome =
    std::variant<CheckedIntegerValue, CheckedIntegerFailure>;

[[nodiscard]] inline bool
validCheckedIntegerDomain(CheckedIntegerDomain domain) {
  return domain.width > 0 && domain.width <= 64;
}

[[nodiscard]] inline CheckedIntegerValue
normalizeCheckedInteger(CheckedIntegerValue value) {
  if (value.magnitude == 0) {
    value.negative = false;
  }
  return value;
}

[[nodiscard]] inline std::uint64_t
checkedIntegerMask(CheckedIntegerDomain domain) {
  if (!validCheckedIntegerDomain(domain)) {
    return 0;
  }
  return domain.width == 64 ? std::numeric_limits<std::uint64_t>::max()
                            : (std::uint64_t{1} << domain.width) - 1;
}

[[nodiscard]] inline bool checkedIntegerFits(CheckedIntegerValue value,
                                             CheckedIntegerDomain domain) {
  if (!validCheckedIntegerDomain(domain)) {
    return false;
  }
  value = normalizeCheckedInteger(value);
  if (!domain.signedValue) {
    return !value.negative && value.magnitude <= checkedIntegerMask(domain);
  }

  const std::uint64_t negativeLimit = std::uint64_t{1} << (domain.width - 1);
  return value.negative ? value.magnitude <= negativeLimit
                        : value.magnitude < negativeLimit;
}

// Evaluates one checked unary/binary operation. Returns std::nullopt for an
// invalid request (bad domain, operand outside the domain, or an operation
// the domain does not support, such as negating an unsigned value); returns
// a CheckedIntegerFailure inside the outcome for a valid request whose
// result fails GTI's checked rules. Shifts validate their count and then
// operate on the two's-complement bit pattern without an overflow check;
// remainder follows the dividend's sign.
//
// Implemented with llvm::APInt in src/compiler/checked_integer.cpp. LLVM stays
// private to the compiled implementation; this public contract intentionally
// exposes no LLVM types.
[[nodiscard]] std::optional<CheckedIntegerOutcome>
evaluateCheckedIntegerUnary(CheckedIntegerOperation operation,
                            CheckedIntegerValue operand,
                            CheckedIntegerDomain domain);

[[nodiscard]] std::optional<CheckedIntegerOutcome> evaluateCheckedIntegerBinary(
    CheckedIntegerOperation operation, CheckedIntegerValue left,
    CheckedIntegerValue right, CheckedIntegerDomain domain);

// Evaluates explicit non-failing add, subtract, or multiply in one fixed-width
// integer domain. Wrapping mode returns the low domain.width bits. Saturating
// mode clamps the mathematical result to the nearest domain endpoint. Invalid
// domains, out-of-domain operands, and unsupported operations return nullopt.
//
// Implemented with llvm::APInt in src/compiler/checked_integer.cpp. As with the
// checked evaluator above, LLVM remains private implementation machinery.
[[nodiscard]] std::optional<CheckedIntegerValue> evaluateDefinedIntegerBinary(
    CheckedIntegerOperation operation, CheckedIntegerValue left,
    CheckedIntegerValue right, CheckedIntegerDomain domain,
    IntegerArithmeticMode mode);

} // namespace lang
