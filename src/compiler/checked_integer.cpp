#include "gti/checked_integer.h"

#include "llvm/ADT/APInt.h"

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

[[nodiscard]] llvm::APInt saturatedValue(CheckedIntegerOperation operation,
                                         const llvm::APInt &left,
                                         const llvm::APInt &right,
                                         CheckedIntegerDomain domain) {
  if (!domain.signedValue) {
    return operation == CheckedIntegerOperation::Subtract
               ? llvm::APInt::getZero(domain.width)
               : llvm::APInt::getMaxValue(domain.width);
  }

  bool clampToMinimum = false;
  switch (operation) {
  case CheckedIntegerOperation::Add:
    clampToMinimum = left.isNegative();
    break;
  case CheckedIntegerOperation::Subtract:
    clampToMinimum = left.isNegative() && !right.isNegative();
    break;
  case CheckedIntegerOperation::Multiply:
    clampToMinimum = left.isNegative() != right.isNegative();
    break;
  default:
    break;
  }
  return clampToMinimum ? llvm::APInt::getSignedMinValue(domain.width)
                        : llvm::APInt::getSignedMaxValue(domain.width);
}

} // namespace

std::optional<CheckedIntegerOutcome>
evaluateCheckedIntegerUnary(CheckedIntegerOperation operation,
                            CheckedIntegerValue operand,
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

std::optional<CheckedIntegerValue> evaluateDefinedIntegerBinary(
    CheckedIntegerOperation operation, CheckedIntegerValue left,
    CheckedIntegerValue right, CheckedIntegerDomain domain,
    IntegerArithmeticMode mode) {
  if (!validCheckedIntegerDomain(domain) || !checkedIntegerFits(left, domain) ||
      !checkedIntegerFits(right, domain) ||
      mode == IntegerArithmeticMode::CheckedResult) {
    return std::nullopt;
  }

  const llvm::APInt lhs = toAPInt(left, domain);
  const llvm::APInt rhs = toAPInt(right, domain);
  llvm::APInt result = llvm::APInt::getZero(domain.width);
  bool overflow = false;
  switch (operation) {
  case CheckedIntegerOperation::Add:
    result = domain.signedValue ? lhs.sadd_ov(rhs, overflow)
                                : lhs.uadd_ov(rhs, overflow);
    break;
  case CheckedIntegerOperation::Subtract:
    result = domain.signedValue ? lhs.ssub_ov(rhs, overflow)
                                : lhs.usub_ov(rhs, overflow);
    break;
  case CheckedIntegerOperation::Multiply:
    result = domain.signedValue ? lhs.smul_ov(rhs, overflow)
                                : lhs.umul_ov(rhs, overflow);
    break;
  default:
    return std::nullopt;
  }

  if (mode == IntegerArithmeticMode::Saturating && overflow) {
    result = saturatedValue(operation, lhs, rhs, domain);
  }
  return fromAPInt(result, domain);
}

} // namespace lang
