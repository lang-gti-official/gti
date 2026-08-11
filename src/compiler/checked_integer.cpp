#include "gti/checked_integer.h"

#include <limits>

#if GTI_HAS_LLVM
#include "llvm/ADT/APInt.h"
#endif

namespace lang {

namespace {

// Shared request validation. Both implementations reject the same invalid
// requests with std::nullopt before any arithmetic runs.

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

// --- Portable reference implementation -----------------------------------

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

#if GTI_HAS_LLVM

// --- llvm::APInt implementation ------------------------------------------
//
// Two's-complement arithmetic in exactly `domain.width` bits with LLVM's
// overflow-reporting operations. Must agree outcome-for-outcome with the
// portable reference; the differential test in optimizer_foundation is the
// standing proof.

namespace {

[[nodiscard]] llvm::APInt toAPInt(CheckedIntegerValue value,
                                  CheckedIntegerDomain domain) {
  value = normalizeCheckedInteger(value);
  llvm::APInt result(domain.width, value.magnitude);
  return value.negative ? -result : result;
}

[[nodiscard]] CheckedIntegerValue fromAPInt(const llvm::APInt &value,
                                            CheckedIntegerDomain domain) {
  if (domain.signedValue && value.isNegative()) {
    return normalizeCheckedInteger(
        {.negative = true, .magnitude = (-value).getZExtValue()});
  }
  return {.magnitude = value.getZExtValue()};
}

[[nodiscard]] CheckedIntegerOutcome apintOutcome(const llvm::APInt &value,
                                                 bool overflow,
                                                 CheckedIntegerDomain domain) {
  if (overflow) {
    return CheckedIntegerFailure::Overflow;
  }
  return fromAPInt(value, domain);
}

std::optional<CheckedIntegerOutcome>
apintUnary(CheckedIntegerOperation operation, CheckedIntegerValue operand,
           CheckedIntegerDomain domain) {
  if (!validCheckedIntegerDomain(domain) ||
      !checkedIntegerFits(operand, domain)) {
    return std::nullopt;
  }

  const llvm::APInt value = toAPInt(operand, domain);
  switch (operation) {
  case CheckedIntegerOperation::Negate: {
    if (!domain.signedValue) {
      return std::nullopt;
    }
    bool overflow = false;
    const llvm::APInt negated =
        llvm::APInt::getZero(domain.width).ssub_ov(value, overflow);
    return apintOutcome(negated, overflow, domain);
  }
  case CheckedIntegerOperation::BitwiseNot:
    return CheckedIntegerOutcome{fromAPInt(~value, domain)};
  default:
    return std::nullopt;
  }
}

std::optional<CheckedIntegerOutcome>
apintBinary(CheckedIntegerOperation operation, CheckedIntegerValue left,
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
    const unsigned count = static_cast<unsigned>(right.magnitude);
    const llvm::APInt bits = toAPInt(left, domain);
    if (operation == CheckedIntegerOperation::ShiftLeft) {
      return CheckedIntegerOutcome{fromAPInt(bits.shl(count), domain)};
    }
    return CheckedIntegerOutcome{fromAPInt(
        domain.signedValue ? bits.ashr(count) : bits.lshr(count), domain)};
  }

  if (!checkedIntegerFits(right, domain)) {
    return std::nullopt;
  }

  const llvm::APInt lhs = toAPInt(left, domain);
  const llvm::APInt rhs = toAPInt(right, domain);
  bool overflow = false;
  switch (operation) {
  case CheckedIntegerOperation::Add: {
    const llvm::APInt sum = domain.signedValue ? lhs.sadd_ov(rhs, overflow)
                                               : lhs.uadd_ov(rhs, overflow);
    return apintOutcome(sum, overflow, domain);
  }
  case CheckedIntegerOperation::Subtract: {
    const llvm::APInt difference = domain.signedValue
                                       ? lhs.ssub_ov(rhs, overflow)
                                       : lhs.usub_ov(rhs, overflow);
    return apintOutcome(difference, overflow, domain);
  }
  case CheckedIntegerOperation::Multiply: {
    const llvm::APInt product = domain.signedValue ? lhs.smul_ov(rhs, overflow)
                                                   : lhs.umul_ov(rhs, overflow);
    return apintOutcome(product, overflow, domain);
  }
  case CheckedIntegerOperation::Divide: {
    if (rhs.isZero()) {
      return CheckedIntegerOutcome{CheckedIntegerFailure::DivisionByZero};
    }
    if (!domain.signedValue) {
      return CheckedIntegerOutcome{fromAPInt(lhs.udiv(rhs), domain)};
    }
    const llvm::APInt quotient = lhs.sdiv_ov(rhs, overflow);
    return apintOutcome(quotient, overflow, domain);
  }
  case CheckedIntegerOperation::Remainder:
    if (rhs.isZero()) {
      return CheckedIntegerOutcome{CheckedIntegerFailure::ModuloByZero};
    }
    return CheckedIntegerOutcome{
        fromAPInt(domain.signedValue ? lhs.srem(rhs) : lhs.urem(rhs), domain)};
  case CheckedIntegerOperation::BitwiseAnd:
    return CheckedIntegerOutcome{fromAPInt(lhs & rhs, domain)};
  case CheckedIntegerOperation::BitwiseOr:
    return CheckedIntegerOutcome{fromAPInt(lhs | rhs, domain)};
  case CheckedIntegerOperation::BitwiseXor:
    return CheckedIntegerOutcome{fromAPInt(lhs ^ rhs, domain)};
  case CheckedIntegerOperation::ShiftLeft:
  case CheckedIntegerOperation::ShiftRight:
  case CheckedIntegerOperation::Negate:
  case CheckedIntegerOperation::BitwiseNot:
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace

#endif

std::optional<CheckedIntegerOutcome>
evaluateCheckedIntegerUnary(CheckedIntegerOperation operation,
                            CheckedIntegerValue operand,
                            CheckedIntegerDomain domain) {
#if GTI_HAS_LLVM
  return apintUnary(operation, operand, domain);
#else
  return portable::evaluateCheckedIntegerUnary(operation, operand, domain);
#endif
}

std::optional<CheckedIntegerOutcome> evaluateCheckedIntegerBinary(
    CheckedIntegerOperation operation, CheckedIntegerValue left,
    CheckedIntegerValue right, CheckedIntegerDomain domain) {
#if GTI_HAS_LLVM
  return apintBinary(operation, left, right, domain);
#else
  return portable::evaluateCheckedIntegerBinary(operation, left, right, domain);
#endif
}

} // namespace lang
