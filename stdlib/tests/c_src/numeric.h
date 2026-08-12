#pragma once

#include <numeric>

namespace stdlib::tests::numeric {
extern "C" {
int stdlib_cpp_gcd(int left, int right);
int stdlib_cpp_lcd(int left, int right);
}
} // namespace stdlib::tests::numeric
