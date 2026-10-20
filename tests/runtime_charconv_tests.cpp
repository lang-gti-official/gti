#include "gti/charconv.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
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

std::string formattedDouble(double value, uint32_t format, int32_t precision,
                            uint32_t flags = 0, uint32_t fill = ' ',
                            uint32_t alignment = 0, uint32_t sign = '-',
                            uint32_t zeroPad = 0, uint64_t width = 0) {
  std::array<uint8_t, 128> output{};
  uint64_t size = 0;
  expect(gti_rt_format_f64_v1(value, format, precision, flags, fill, alignment,
                              sign, zeroPad, width, output.data(),
                              output.size(), &size) == 0,
         "binary64 formatting succeeds");
  return {reinterpret_cast<const char *>(output.data()),
          static_cast<std::size_t>(size)};
}
} // namespace

void testFloatParser();

int main() {
  testFloatParser();
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

  expect(formattedDouble(1.5, 0, -1) == "1.5",
         "general binary64 formatting uses the shortest representation");
  expect(formattedDouble(1.25, 1, 3) == "1.250", "fixed precision is honored");
  expect(formattedDouble(12.5, 2, 2, 1) == "1.25E+01",
         "scientific formatting can be uppercased");
  expect(formattedDouble(12.0, 0, 5, 2 | 4) == "12.000",
         "alternate general formatting retains significant trailing zeroes");
  expect(formattedDouble(1.5, 0, -1, 2 | 4) == "1.5" &&
             formattedDouble(0.0, 0, -1, 2 | 4) == "0.",
         "alternate default formatting preserves the shortest digits and "
         "adds only a required decimal point");
  expect(formattedDouble(1.5, 0, 6, 2 | 4) == "1.50000",
         "explicit alternate general precision retains significant zeroes");
  expect(formattedDouble(-3.5, 1, 2, 0, ' ', 0, '-', 1, 10) == "-000003.50",
         "zero padding follows the numeric sign");
  expect(formattedDouble(std::numeric_limits<double>::infinity(), 0, -1, 4, ' ',
                         0, '-', 1, 8) == "     inf" &&
             formattedDouble(-std::numeric_limits<double>::infinity(), 0, -1, 4,
                             ' ', 0, '-', 1, 8) == "    -inf" &&
             formattedDouble(std::numeric_limits<double>::quiet_NaN(), 0, -1, 4,
                             ' ', 0, '-', 1, 8) == "     nan",
         "infinity and NaN ignore numeric zero padding");
  expect(formattedDouble(7.0, 1, 1, 0, '*', '^', '-', 0, 7) == "**7.0**",
         "custom centered fill is balanced");

  std::array<uint8_t, 4> small{};
  uint64_t size = 77;
  expect(gti_rt_format_f64_v1(1.25, 1, 2, 0, ' ', 0, '-', 0, 0, small.data(), 3,
                              &size) == 2 &&
             size == 77,
         "too-small format buffers preserve the reported size");
  expect(gti_rt_format_f64_v1(1.25, 4, 2, 0, ' ', 0, '-', 0, 0, small.data(),
                              small.size(), &size) == 1 &&
             size == 77,
         "invalid formatting requests preserve outputs");
  expect(gti_rt_format_f32_v1(1.25F, 1, 2, 0, ' ', 0, '-', 0, 0, small.data(),
                              small.size(), &size) == 0 &&
             size == 4 &&
             std::string_view(reinterpret_cast<const char *>(small.data()),
                              static_cast<std::size_t>(size)) == "1.25",
         "binary32 uses the same bounded formatting ABI");
}
