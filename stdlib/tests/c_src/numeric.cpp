#include <numeric>

extern "C" {
int stdlib_cpp_gcd(int left, int right) { return std::gcd(left, right); }
int stdlib_cpp_lcm(int left, int right) { return std::lcm(left, right); }
}
