#pragma once

#include "gti/checked_integer.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace lang {

// GTI's `float` value is an IEEE-754 binary32 bit pattern. LLVM implements the
// computations in src/compiler/binary_float.cpp, but this GTI-owned value is
// the representation retained by tokens, semantic constants, HIR, and MIR.
struct BinaryFloat {
  std::uint32_t bits = 0;

  friend bool operator==(BinaryFloat, BinaryFloat) = default;
};

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

// Parses the decimal floating-literal grammar directly into binary32 using
// round-to-nearest, ties-to-even. Overflow is rejected; underflow produces the
// correctly rounded subnormal or signed zero.
[[nodiscard]] BinaryFloatParseResult
parseBinaryFloat(std::string_view spelling);

// IEEE-754 operations use round-to-nearest, ties-to-even and default exception
// results. In particular division by zero produces infinity or NaN rather than
// an integer-style evaluator failure.
[[nodiscard]] BinaryFloat evaluateBinaryFloat(BinaryFloatOperation operation,
                                              BinaryFloat left,
                                              BinaryFloat right);
[[nodiscard]] BinaryFloat negateBinaryFloat(BinaryFloat value);
[[nodiscard]] BinaryFloatOrdering compareBinaryFloat(BinaryFloat left,
                                                     BinaryFloat right);

// Numeric conversions share the same checked boundaries as emitted runtime
// conversions. Floating-to-integer conversion truncates toward zero and
// returns nullopt for NaN, infinity, or a value outside the target domain.
[[nodiscard]] std::optional<BinaryFloat>
integerToBinaryFloat(CheckedIntegerValue value, CheckedIntegerDomain domain);
[[nodiscard]] std::optional<CheckedIntegerValue>
binaryFloatToInteger(BinaryFloat value, CheckedIntegerDomain domain);

} // namespace lang
