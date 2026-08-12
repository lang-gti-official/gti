#include "numeric.h"

namespace stdlib::tests::numeric {
int stdlib_cpp_gcd(int left, int right) { return std::gcd(left, right); }
int stdlib_cpp_lcd(int left, int right) { return std::lcm(left, right); }
} // namespace stdlib::tests::numeric
