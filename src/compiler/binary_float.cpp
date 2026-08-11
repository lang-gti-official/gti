#include "gti/binary_float.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <algorithm>

namespace lang {
namespace {

constexpr llvm::APFloat::roundingMode roundingMode =
    llvm::APFloat::rmNearestTiesToEven;

[[nodiscard]] bool isDecimalFloatSpelling(std::string_view spelling) {
  const std::size_t point = spelling.find('.');
  if (point == std::string_view::npos || point == 0 ||
      point + 1 == spelling.size() ||
      spelling.find('.', point + 1) != std::string_view::npos) {
    return false;
  }
  return std::all_of(spelling.begin(), spelling.end(), [](char character) {
    return (character >= '0' && character <= '9') || character == '.';
  });
}

[[nodiscard]] llvm::APFloat toAPFloat(BinaryFloat value) {
  return llvm::APFloat(llvm::APFloat::IEEEsingle(),
                       llvm::APInt(32, value.bits));
}

[[nodiscard]] BinaryFloat fromAPFloat(const llvm::APFloat &value) {
  return {static_cast<std::uint32_t>(value.bitcastToAPInt().getZExtValue())};
}

} // namespace

BinaryFloatParseResult parseBinaryFloat(std::string_view spelling) {
  if (!isDecimalFloatSpelling(spelling)) {
    return {.failure = BinaryFloatParseFailure::Invalid};
  }

  llvm::APFloat value = llvm::APFloat::getZero(llvm::APFloat::IEEEsingle());
  llvm::Expected<llvm::APFloat::opStatus> parsed = value.convertFromString(
      llvm::StringRef(spelling.data(), spelling.size()), roundingMode);
  if (!parsed) {
    llvm::consumeError(parsed.takeError());
    return {.failure = BinaryFloatParseFailure::Invalid};
  }
  if ((*parsed & llvm::APFloat::opOverflow) != 0) {
    return {.failure = BinaryFloatParseFailure::OutOfRange};
  }
  return {.value = fromAPFloat(value)};
}

BinaryFloat evaluateBinaryFloat(BinaryFloatOperation operation,
                                BinaryFloat left, BinaryFloat right) {
  llvm::APFloat result = toAPFloat(left);
  const llvm::APFloat operand = toAPFloat(right);
  switch (operation) {
  case BinaryFloatOperation::Add:
    (void)result.add(operand, roundingMode);
    break;
  case BinaryFloatOperation::Subtract:
    (void)result.subtract(operand, roundingMode);
    break;
  case BinaryFloatOperation::Multiply:
    (void)result.multiply(operand, roundingMode);
    break;
  case BinaryFloatOperation::Divide:
    (void)result.divide(operand, roundingMode);
    break;
  }
  return fromAPFloat(result);
}

BinaryFloat negateBinaryFloat(BinaryFloat value) {
  llvm::APFloat result = toAPFloat(value);
  result.changeSign();
  return fromAPFloat(result);
}

BinaryFloatOrdering compareBinaryFloat(BinaryFloat left, BinaryFloat right) {
  switch (toAPFloat(left).compare(toAPFloat(right))) {
  case llvm::APFloat::cmpLessThan:
    return BinaryFloatOrdering::Less;
  case llvm::APFloat::cmpEqual:
    return BinaryFloatOrdering::Equal;
  case llvm::APFloat::cmpGreaterThan:
    return BinaryFloatOrdering::Greater;
  case llvm::APFloat::cmpUnordered:
    return BinaryFloatOrdering::Unordered;
  }
  return BinaryFloatOrdering::Unordered;
}

std::optional<BinaryFloat> integerToBinaryFloat(CheckedIntegerValue value,
                                                CheckedIntegerDomain domain) {
  value = normalizeCheckedInteger(value);
  if (!validCheckedIntegerDomain(domain) ||
      !checkedIntegerFits(value, domain)) {
    return std::nullopt;
  }

  llvm::APInt integer(domain.width, value.magnitude);
  if (value.negative) {
    integer = -integer;
  }
  llvm::APFloat result = llvm::APFloat::getZero(llvm::APFloat::IEEEsingle());
  (void)result.convertFromAPInt(integer, domain.signedValue, roundingMode);
  return fromAPFloat(result);
}

std::optional<CheckedIntegerValue>
binaryFloatToInteger(BinaryFloat value, CheckedIntegerDomain domain) {
  if (!validCheckedIntegerDomain(domain)) {
    return std::nullopt;
  }

  llvm::APSInt integer(domain.width, !domain.signedValue);
  bool exact = false;
  const llvm::APFloat::opStatus status = toAPFloat(value).convertToInteger(
      integer, llvm::APFloat::rmTowardZero, &exact);
  (void)exact;
  if ((status & llvm::APFloat::opInvalidOp) != 0) {
    return std::nullopt;
  }

  if (domain.signedValue && integer.isNegative()) {
    return normalizeCheckedInteger(
        {.negative = true, .magnitude = (-integer).getZExtValue()});
  }
  return CheckedIntegerValue{.magnitude = integer.getZExtValue()};
}

} // namespace lang
