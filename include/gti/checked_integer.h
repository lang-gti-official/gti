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

namespace checked_integer_detail {

[[nodiscard]] inline CheckedIntegerOutcome
checkedResult(CheckedIntegerValue value, CheckedIntegerDomain domain) {
  value = normalizeCheckedInteger(value);
  if (!checkedIntegerFits(value, domain)) {
    return CheckedIntegerFailure::Overflow;
  }
  return value;
}

[[nodiscard]] inline CheckedIntegerOutcome add(CheckedIntegerValue left,
                                               CheckedIntegerValue right,
                                               CheckedIntegerDomain domain) {
  left = normalizeCheckedInteger(left);
  right = normalizeCheckedInteger(right);
  if (left.negative == right.negative) {
    if (left.magnitude >
        std::numeric_limits<std::uint64_t>::max() - right.magnitude) {
      return CheckedIntegerFailure::Overflow;
    }
    return checkedResult({.negative = left.negative,
                          .magnitude = left.magnitude + right.magnitude},
                         domain);
  }

  if (left.magnitude >= right.magnitude) {
    return checkedResult({.negative = left.negative,
                          .magnitude = left.magnitude - right.magnitude},
                         domain);
  }
  return checkedResult({.negative = right.negative,
                        .magnitude = right.magnitude - left.magnitude},
                       domain);
}

[[nodiscard]] inline std::uint64_t toBitPattern(CheckedIntegerValue value,
                                                CheckedIntegerDomain domain) {
  value = normalizeCheckedInteger(value);
  const std::uint64_t mask = checkedIntegerMask(domain);
  if (!value.negative) {
    return value.magnitude & mask;
  }
  return (std::uint64_t{0} - value.magnitude) & mask;
}

[[nodiscard]] inline CheckedIntegerValue
fromBitPattern(std::uint64_t bits, CheckedIntegerDomain domain) {
  const std::uint64_t mask = checkedIntegerMask(domain);
  bits &= mask;
  if (!domain.signedValue) {
    return {.magnitude = bits};
  }

  const std::uint64_t signBit = std::uint64_t{1} << (domain.width - 1);
  if ((bits & signBit) == 0) {
    return {.magnitude = bits};
  }
  return normalizeCheckedInteger(
      {.negative = true, .magnitude = (std::uint64_t{0} - bits) & mask});
}

[[nodiscard]] inline std::optional<CheckedIntegerFailure>
validateShiftCount(CheckedIntegerValue count, CheckedIntegerDomain domain) {
  count = normalizeCheckedInteger(count);
  if (count.negative) {
    return CheckedIntegerFailure::NegativeShiftCount;
  }
  if (count.magnitude >= domain.width) {
    return CheckedIntegerFailure::ShiftCountOutOfRange;
  }
  return std::nullopt;
}

} // namespace checked_integer_detail

[[nodiscard]] inline std::optional<CheckedIntegerOutcome>
evaluateCheckedIntegerUnary(CheckedIntegerOperation operation,
                            CheckedIntegerValue operand,
                            CheckedIntegerDomain domain) {
  if (!validCheckedIntegerDomain(domain) ||
      !checkedIntegerFits(operand, domain)) {
    return std::nullopt;
  }

  switch (operation) {
  case CheckedIntegerOperation::Negate: {
    if (!domain.signedValue) {
      return std::nullopt;
    }
    operand = normalizeCheckedInteger(operand);
    operand.negative = operand.magnitude != 0 && !operand.negative;
    return checked_integer_detail::checkedResult(operand, domain);
  }
  case CheckedIntegerOperation::BitwiseNot: {
    const std::uint64_t bits =
        ~checked_integer_detail::toBitPattern(operand, domain) &
        checkedIntegerMask(domain);
    return CheckedIntegerOutcome{
        checked_integer_detail::fromBitPattern(bits, domain)};
  }
  default:
    return std::nullopt;
  }
}

[[nodiscard]] inline std::optional<CheckedIntegerOutcome>
evaluateCheckedIntegerBinary(CheckedIntegerOperation operation,
                             CheckedIntegerValue left,
                             CheckedIntegerValue right,
                             CheckedIntegerDomain domain) {
  if (!validCheckedIntegerDomain(domain) || !checkedIntegerFits(left, domain)) {
    return std::nullopt;
  }

  if (operation == CheckedIntegerOperation::ShiftLeft ||
      operation == CheckedIntegerOperation::ShiftRight) {
    if (const std::optional<CheckedIntegerFailure> failure =
            checked_integer_detail::validateShiftCount(right, domain)) {
      return CheckedIntegerOutcome{*failure};
    }

    const std::uint8_t count = static_cast<std::uint8_t>(right.magnitude);
    const std::uint64_t mask = checkedIntegerMask(domain);
    const std::uint64_t bits =
        checked_integer_detail::toBitPattern(left, domain);
    if (operation == CheckedIntegerOperation::ShiftLeft) {
      return CheckedIntegerOutcome{checked_integer_detail::fromBitPattern(
          (bits << count) & mask, domain)};
    }

    std::uint64_t shifted = bits >> count;
    const std::uint64_t signBit = std::uint64_t{1} << (domain.width - 1);
    if (domain.signedValue && count != 0 && (bits & signBit) != 0) {
      const std::uint8_t retainedWidth = domain.width - count;
      shifted |= mask ^ ((std::uint64_t{1} << retainedWidth) - 1);
    }
    return CheckedIntegerOutcome{
        checked_integer_detail::fromBitPattern(shifted, domain)};
  }

  if (!checkedIntegerFits(right, domain)) {
    return std::nullopt;
  }
  left = normalizeCheckedInteger(left);
  right = normalizeCheckedInteger(right);

  switch (operation) {
  case CheckedIntegerOperation::Add:
    return checked_integer_detail::add(left, right, domain);
  case CheckedIntegerOperation::Subtract:
    if (right.magnitude != 0) {
      right.negative = !right.negative;
    }
    return checked_integer_detail::add(left, right, domain);
  case CheckedIntegerOperation::Multiply: {
    if (right.magnitude != 0 &&
        left.magnitude >
            std::numeric_limits<std::uint64_t>::max() / right.magnitude) {
      return CheckedIntegerOutcome{CheckedIntegerFailure::Overflow};
    }
    return checked_integer_detail::checkedResult(
        {.negative = left.negative != right.negative,
         .magnitude = left.magnitude * right.magnitude},
        domain);
  }
  case CheckedIntegerOperation::Divide:
    if (right.magnitude == 0) {
      return CheckedIntegerOutcome{CheckedIntegerFailure::DivisionByZero};
    }
    return checked_integer_detail::checkedResult(
        {.negative = left.negative != right.negative,
         .magnitude = left.magnitude / right.magnitude},
        domain);
  case CheckedIntegerOperation::Remainder:
    if (right.magnitude == 0) {
      return CheckedIntegerOutcome{CheckedIntegerFailure::ModuloByZero};
    }
    return CheckedIntegerOutcome{normalizeCheckedInteger(
        {.negative = left.negative,
         .magnitude = left.magnitude % right.magnitude})};
  case CheckedIntegerOperation::BitwiseAnd:
  case CheckedIntegerOperation::BitwiseOr:
  case CheckedIntegerOperation::BitwiseXor: {
    const std::uint64_t leftBits =
        checked_integer_detail::toBitPattern(left, domain);
    const std::uint64_t rightBits =
        checked_integer_detail::toBitPattern(right, domain);
    const std::uint64_t result =
        operation == CheckedIntegerOperation::BitwiseAnd ? leftBits & rightBits
        : operation == CheckedIntegerOperation::BitwiseOr
            ? leftBits | rightBits
            : leftBits ^ rightBits;
    return CheckedIntegerOutcome{
        checked_integer_detail::fromBitPattern(result, domain)};
  }
  case CheckedIntegerOperation::ShiftLeft:
  case CheckedIntegerOperation::ShiftRight:
  case CheckedIntegerOperation::Negate:
  case CheckedIntegerOperation::BitwiseNot:
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace lang
