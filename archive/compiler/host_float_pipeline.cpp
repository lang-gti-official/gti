// ARCHIVED: excluded from the GTI build. See archive/compiler/README.md.
//
// This preserves the host-double path displaced by the compiler-owned
// binary32/APFloat implementation. It intentionally is not wired to current
// GTI headers and must not become a selectable fallback.

#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace lang::archived_host_float {

[[nodiscard]] std::optional<double> parse(const std::string &text) {
  try {
    return std::stod(text);
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

[[nodiscard]] double negate(double value) { return -value; }

enum class Ordering { Less, Equal, Greater, Unordered };

[[nodiscard]] Ordering compare(double left, double right) {
  if (std::isnan(left) || std::isnan(right)) {
    return Ordering::Unordered;
  }
  if (left < right) {
    return Ordering::Less;
  }
  if (left > right) {
    return Ordering::Greater;
  }
  return Ordering::Equal;
}

[[nodiscard]] std::string emit(double value) {
  std::ostringstream literal;
  literal << std::showpoint
          << std::setprecision(std::numeric_limits<float>::max_digits10)
          << static_cast<float>(value);
  return literal.str() + 'F';
}

} // namespace lang::archived_host_float
