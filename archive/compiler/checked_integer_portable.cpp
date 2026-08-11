// ARCHIVED: excluded from the GTI build. See archive/compiler/README.md.

#include "gti/checked_integer.h"

#include <limits>

namespace lang {

namespace {

[[nodiscard]] std::optional<CheckedIntegerFailure>
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

[[nodiscard]] CheckedIntegerOutcome checkedResult(CheckedIntegerValue value,
                                                  CheckedIntegerDomain domain) {
  value = normalizeCheckedInteger(value);
  if (!checkedIntegerFits(value, domain)) {
    return CheckedIntegerFailure::Overflow;
  }
  return value;
}

[[nodiscard]] CheckedIntegerOutcome portableAdd(CheckedIntegerValue left,
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

[[nodiscard]] std::uint64_t toBitPattern(CheckedIntegerValue value,
                                         CheckedIntegerDomain domain) {
  value = normalizeCheckedInteger(value);
  const std::uint64_t mask = checkedIntegerMask(domain);
  if (!value.negative) {
    return value.magnitude & mask;
  }
  return (std::uint64_t{0} - value.magnitude) & mask;
}

[[nodiscard]] CheckedIntegerValue fromBitPattern(std::uint64_t bits,
                                                 CheckedIntegerDomain domain) {
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

} // namespace

namespace portable {

std::optional<CheckedIntegerOutcome>
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
    return checkedResult(operand, domain);
  }
  case CheckedIntegerOperation::BitwiseNot: {
    const std::uint64_t bits =
        ~toBitPattern(operand, domain) & checkedIntegerMask(domain);
    return CheckedIntegerOutcome{fromBitPattern(bits, domain)};
  }
  default:
    return std::nullopt;
  }
}

std::optional<CheckedIntegerOutcome> evaluateCheckedIntegerBinary(
    CheckedIntegerOperation operation, CheckedIntegerValue left,
    CheckedIntegerValue right, CheckedIntegerDomain domain) {
  if (!validCheckedIntegerDomain(domain) || !checkedIntegerFits(left, domain)) {
    return std::nullopt;
  }

  if (operation == CheckedIntegerOperation::ShiftLeft ||
      operation == CheckedIntegerOperation::ShiftRight) {
    if (const std::optional<CheckedIntegerFailure> failure =
            validateShiftCount(right, domain)) {
      return CheckedIntegerOutcome{*failure};
    }

    const std::uint8_t count = static_cast<std::uint8_t>(right.magnitude);
    const std::uint64_t mask = checkedIntegerMask(domain);
    const std::uint64_t bits = toBitPattern(left, domain);
    if (operation == CheckedIntegerOperation::ShiftLeft) {
      return CheckedIntegerOutcome{
          fromBitPattern((bits << count) & mask, domain)};
    }

    std::uint64_t shifted = bits >> count;
    const std::uint64_t signBit = std::uint64_t{1} << (domain.width - 1);
    if (domain.signedValue && count != 0 && (bits & signBit) != 0) {
      const std::uint8_t retainedWidth = domain.width - count;
      shifted |= mask ^ ((std::uint64_t{1} << retainedWidth) - 1);
    }
    return CheckedIntegerOutcome{fromBitPattern(shifted, domain)};
  }

  if (!checkedIntegerFits(right, domain)) {
    return std::nullopt;
  }
  left = normalizeCheckedInteger(left);
  right = normalizeCheckedInteger(right);

  switch (operation) {
  case CheckedIntegerOperation::Add:
    return portableAdd(left, right, domain);
  case CheckedIntegerOperation::Subtract:
    if (right.magnitude != 0) {
      right.negative = !right.negative;
    }
    return portableAdd(left, right, domain);
  case CheckedIntegerOperation::Multiply: {
    if (right.magnitude != 0 &&
        left.magnitude >
            std::numeric_limits<std::uint64_t>::max() / right.magnitude) {
      return CheckedIntegerOutcome{CheckedIntegerFailure::Overflow};
    }
    return checkedResult({.negative = left.negative != right.negative,
                          .magnitude = left.magnitude * right.magnitude},
                         domain);
  }
  case CheckedIntegerOperation::Divide:
    if (right.magnitude == 0) {
      return CheckedIntegerOutcome{CheckedIntegerFailure::DivisionByZero};
    }
    return checkedResult({.negative = left.negative != right.negative,
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
    const std::uint64_t leftBits = toBitPattern(left, domain);
    const std::uint64_t rightBits = toBitPattern(right, domain);
    const std::uint64_t result =
        operation == CheckedIntegerOperation::BitwiseAnd ? leftBits & rightBits
        : operation == CheckedIntegerOperation::BitwiseOr
            ? leftBits | rightBits
            : leftBits ^ rightBits;
    return CheckedIntegerOutcome{fromBitPattern(result, domain)};
  }
  case CheckedIntegerOperation::ShiftLeft:
  case CheckedIntegerOperation::ShiftRight:
  case CheckedIntegerOperation::Negate:
  case CheckedIntegerOperation::BitwiseNot:
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace portable

} // namespace lang
