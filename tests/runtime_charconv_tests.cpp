#include "gti/charconv.h"

#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
gti_c_string_view view(std::string_view value) {
  return {value.data(), value.size()};
}
} // namespace

int main() {
  double value = 7;
  float narrow = 7;
  uint64_t position = 99;
  expect(gti_rt_parse_f64_v1({nullptr, 1}, 0, 0, &value, &position) == 1 &&
             value == 7 && position == 99,
         "reject invalid counted input");
  expect(gti_rt_parse_f64_v1(view("1"), 2, 0, &value, &position) == 1 &&
             position == 99,
         "offset cannot exceed length");
  expect(gti_rt_parse_f64_v1(view("1"), 0, 4, &value, &position) == 1 &&
             value == 7,
         "format selector is bounded");
  expect(gti_rt_parse_f64_v1(view("1"), 0, 0, nullptr, &position) == 1,
         "output is required");
  expect(gti_rt_parse_f64_v1(view("1"), 0, 0, &value, nullptr) == 1,
         "position is required");
  expect(gti_rt_parse_f64_v1({nullptr, 0}, 0, 0, &value, &position) == 1 &&
             position == 0 && value == 7,
         "empty null view is safe");
  expect(gti_rt_parse_f64_v1(view("0.1"), 0, 0, &value, &position) == 0 &&
             std::bit_cast<uint64_t>(value) == 0x3fb999999999999aULL &&
             position == 3,
         "binary64 rounds directly");
  expect(gti_rt_parse_f32_v1(view("0.1"), 0, 0, &narrow, &position) == 0 &&
             std::bit_cast<uint32_t>(narrow) == 0x3dcccccdU,
         "binary32 rounds directly");
  expect(gti_rt_parse_f32_v1(view("1.000000059604644775390625"), 0, 0, &narrow,
                             &position) == 0 &&
             narrow == 1,
         "halfway conversion rounds to nearest even");
  expect(gti_rt_parse_f64_v1(view("5e-324"), 0, 0, &value, &position) == 0 &&
             value == std::numeric_limits<double>::denorm_min(),
         "binary64 subnormal retained");
  expect(gti_rt_parse_f32_v1(view("1.401298464324817e-45"), 0, 0, &narrow,
                             &position) == 0 &&
             narrow == std::numeric_limits<float>::denorm_min(),
         "binary32 subnormal retained");
  value = 7;
  expect(gti_rt_parse_f64_v1(view("1e-999!"), 0, 0, &value, &position) == 2 &&
             value == 7 && position == 6,
         "underflow preserves output and consumes exponent");
  expect(gti_rt_parse_f64_v1(view("1e999!"), 0, 0, &value, &position) == 2 &&
             value == 7 && position == 5,
         "overflow preserves output");
  expect(gti_rt_parse_f64_v1(view("1e+"), 0, 0, &value, &position) == 0 &&
             value == 1 && position == 1,
         "incomplete optional exponent is not consumed");
  expect(gti_rt_parse_f64_v1(view("xx-0!"), 2, 0, &value, &position) == 0 &&
             value == 0 && std::signbit(value) && position == 4,
         "offset and signed zero retained");
  expect(gti_rt_parse_f64_v1(view(std::string_view("12\0.5", 5)), 0, 0, &value,
                             &position) == 0 &&
             value == 12 && position == 2,
         "embedded NUL is an ordinary parse terminator");
}
