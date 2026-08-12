#pragma once

#include "gti/checked_integer.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace lang {

enum class BinaryFloatFormat : std::uint8_t {
  Binary32,
  Binary64,
};

// GTI floating values retain an exact IEEE-754 binary32 or binary64 bit
// pattern. LLVM implements the computations in src/compiler/binary_float.cpp,
// but this GTI-owned value is the representation retained by tokens, semantic
// constants, HIR, and MIR.
struct BinaryFloat {
  std::uint64_t bits = 0;
  BinaryFloatFormat format = BinaryFloatFormat::Binary32;

  friend bool operator==(BinaryFloat, BinaryFloat) = default;
};

[[nodiscard]] constexpr std::uint8_t
binaryFloatWidth(BinaryFloatFormat format) {
  return format == BinaryFloatFormat::Binary32 ? 32 : 64;
}

[[nodiscard]] constexpr bool validBinaryFloat(BinaryFloat value) {
  return value.format == BinaryFloatFormat::Binary64 ||
         (value.bits & 0xffffffff00000000ULL) == 0;
}

enum class BinaryFloatParseFailure {
  None,
  Invalid,
  OutOfRange,
};

struct BinaryFloatParseResult {
  std::optional<BinaryFloat> value;
  BinaryFloatParseFailure failure = BinaryFloatParseFailure::None;

  [[nodiscard]] explicit operator bool() const { return value.has_value(); }
};

enum class BinaryFloatOperation {
  Add,
  Subtract,
  Multiply,
  Divide,
};

enum class BinaryFloatOrdering {
  Less,
  Equal,
  Greater,
  Unordered,
};

// Parses the decimal floating-literal grammar directly into the selected IEEE
// format using round-to-nearest, ties-to-even. Overflow is rejected; underflow
// produces the correctly rounded subnormal or signed zero.
[[nodiscard]] BinaryFloatParseResult
parseBinaryFloat(std::string_view spelling,
                 BinaryFloatFormat format = BinaryFloatFormat::Binary32);

// IEEE-754 operations use round-to-nearest, ties-to-even and default exception
// results. In particular division by zero produces infinity or NaN rather than
// an integer-style evaluator failure.
[[nodiscard]] BinaryFloat evaluateBinaryFloat(BinaryFloatOperation operation,
                                              BinaryFloat left,
                                              BinaryFloat right);
[[nodiscard]] BinaryFloat negateBinaryFloat(BinaryFloat value);
[[nodiscard]] BinaryFloatOrdering compareBinaryFloat(BinaryFloat left,
                                                     BinaryFloat right);
[[nodiscard]] BinaryFloat convertBinaryFloat(BinaryFloat value,
                                             BinaryFloatFormat format);

// Numeric conversions share the same checked boundaries as emitted runtime
// conversions. Floating-to-integer conversion truncates toward zero and
// returns nullopt for NaN, infinity, or a value outside the target domain.
[[nodiscard]] std::optional<BinaryFloat>
integerToBinaryFloat(CheckedIntegerValue value, CheckedIntegerDomain domain,
                     BinaryFloatFormat format = BinaryFloatFormat::Binary32);
[[nodiscard]] std::optional<CheckedIntegerValue>
binaryFloatToInteger(BinaryFloat value, CheckedIntegerDomain domain);

} // namespace lang
