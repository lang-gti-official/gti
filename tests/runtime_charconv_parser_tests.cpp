#include "gti/charconv.h"

#include <array>
#include <bit>
#include <cfenv>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#ifdef GTI_HAS_STD_FLOAT_FROM_CHARS
#include <charconv>
#endif

namespace {
bool countAllocations = false;
std::size_t allocations = 0;

void require(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

template <typename T>
using Bits =
    std::conditional_t<sizeof(T) == sizeof(uint32_t), uint32_t, uint64_t>;

template <typename T>
uint32_t parse(std::string_view input, uint64_t offset, uint32_t format,
               T *value, uint64_t *position) {
  if constexpr (std::is_same_v<T, float>) {
    return gti_rt_parse_f32_v1({input.data(), input.size()}, offset, format,
                               value, position);
  } else {
    return gti_rt_parse_f64_v1({input.data(), input.size()}, offset, format,
                               value, position);
  }
}

template <typename T>
void check(std::string_view input, uint64_t offset, uint32_t format,
           uint32_t expectedError, uint64_t expectedPosition, T expected,
           bool allRoundingModes = true) {
  for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    if (!allRoundingModes && mode != FE_TONEAREST) {
      continue;
    }
    require(std::fesetround(mode) == 0, "set parser rounding mode");
    T value = 7;
    uint64_t position = 99;
    allocations = 0;
    countAllocations = true;
    const auto error = parse(input, offset, format, &value, &position);
    countAllocations = false;
    require(allocations == 0, "floating parsing does not allocate");
    require(std::fegetround() == mode,
            "floating parsing preserves rounding mode");
    const bool equal =
        std::isnan(expected)
            ? std::isnan(value) && std::signbit(value) == std::signbit(expected)
            : std::bit_cast<Bits<T>>(value) == std::bit_cast<Bits<T>>(expected);
    if (error != expectedError || position != expectedPosition || !equal) {
      std::cerr << "parser mismatch: width=" << sizeof(T) * 8
                << " format=" << format << " mode=" << mode
                << " input=" << input.substr(0, 100) << " error=" << error
                << '/' << expectedError << " position=" << position << '/'
                << expectedPosition << " bits=" << std::bit_cast<Bits<T>>(value)
                << '/' << std::bit_cast<Bits<T>>(expected) << '\n';
      std::exit(1);
    }
  }
}

template <typename T> void boundaries() {
  constexpr int precision = std::numeric_limits<T>::digits;
  constexpr int minSubnormal = std::numeric_limits<T>::min_exponent - precision;
  check<T>("xx-0x1.8p2", 2, 0, 0, 4, -T{0});
  check<T>("xx-0x1.8p2", 2, 1, 0, 4, -T{0});
  check<T>("xx-0x1.8p2", 2, 2, 1, 2, T{7});
  check<T>("xx-0x1.8p2", 2, 3, 0, 4, -T{0});
  check<T>("1.8p2!", 0, 3, 0, 5, T{6});
  check<T>("1.5e2", 0, 1, 0, 3, T{1.5});
  check<T>("1.5e2", 0, 2, 0, 5, T{150});
  check<T>("1e+", 0, 0, 0, 1, T{1});
  check<T>("1e+", 0, 2, 1, 0, T{7});
  check<T>(std::string_view("12\0.5", 5), 0, 0, 0, 2, T{12});
  check<T>("NaN(payload_7)!", 0, 0, 0, 14, std::numeric_limits<T>::quiet_NaN());
  check<T>("-INFINITY!", 0, 3, 0, 9, -std::numeric_limits<T>::infinity());
  check<T>("1e99999999999999999999", 0, 0, 2, 22, T{7});
  check<T>("1e-99999999999999999999", 0, 0, 2, 23, T{7});
  check<T>("-0p99999999999999999999", 0, 3, 0, 23, -T{0});
  const auto tiny = "1p" + std::to_string(minSubnormal);
  check<T>(tiny, 0, 3, 0, tiny.size(), std::numeric_limits<T>::denorm_min());
  const auto halfway = "1p" + std::to_string(minSubnormal - 1);
  check<T>(halfway, 0, 3, 2, halfway.size(), T{7});
  const auto aboveHalfway =
      "1.0000000000000001p" + std::to_string(minSubnormal - 1);
  check<T>(aboveHalfway, 0, 3, 0, aboveHalfway.size(),
           std::numeric_limits<T>::denorm_min());
  const std::string longDecimal = "0." + std::string(4096, '0') + "1e4097!";
  check<T>(longDecimal, 0, 0, 0, longDecimal.size() - 1, T{1});
  const std::string longHex = "0." + std::string(4096, '0') + "1p16388!";
  check<T>(longHex, 0, 3, 0, longHex.size() - 1, T{1});
}

#ifdef GTI_HAS_STD_FLOAT_FROM_CHARS
template <typename T>
void differential(std::string_view input, uint32_t format) {
  require(std::fesetround(FE_TONEAREST) == 0, "set oracle rounding mode");
  const auto style = format == 1   ? std::chars_format::fixed
                     : format == 2 ? std::chars_format::scientific
                     : format == 3 ? std::chars_format::hex
                                   : std::chars_format::general;
  T expected = 7;
  auto result = std::from_chars(input.data(), input.data() + input.size(),
                                expected, style);
  const auto error = result.ec == std::errc::result_out_of_range ? 2U
                     : result.ec != std::errc{}                  ? 1U
                                                                 : 0U;
  // The runtime publishes its scalar only on success, independently of the
  // native parser's error-output policy. NaN payload bits are unspecified.
  if (error != 0) {
    expected = 7;
  }
  // Some host libraries observe directed rounding on their decimal fast path.
  // Preserve that existing host behavior; require nearest-even in every fenv
  // mode for the private integer-based implementation, and in the default
  // mode for the native control. Explicit boundaries above exercise both.
#ifdef GTI_TEST_PORTABLE_CHARCONV
  constexpr bool allRoundingModes = true;
#else
  constexpr bool allRoundingModes = false;
#endif
  check<T>(input, 0, format, error, result.ptr - input.data(), expected,
           allRoundingModes);
}
#endif
} // namespace

// Count native C++ allocations in the actual parser, including cold calls.
// The private parser's reachable source uses fixed-size stack storage and no
// malloc-family calls; this catches accidental use of its string helpers.
void *operator new(std::size_t size) {
  if (countAllocations) {
    ++allocations;
  }
  if (void *result = std::malloc(size == 0 ? 1 : size)) {
    return result;
  }
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }

void testFloatParser() {
  const int originalRounding = std::fegetround();
  boundaries<float>();
  boundaries<double>();
  check<float>("1.000000059604644775390625", 0, 0, 0, 26, 1.0F);
  check<float>("1.000001p0", 0, 3, 0, 10, 1.0F);
  check<double>("1.00000000000008p0", 0, 3, 0, 18, 1.0);
  const std::string originalLocale = std::setlocale(LC_NUMERIC, nullptr);
  for (const char *locale : {"C", "de_DE.UTF-8", "fr_FR.UTF-8"}) {
    if (std::setlocale(LC_NUMERIC, locale) != nullptr) {
      check<double>("1.5", 0, 0, 0, 3, 1.5);
      check<double>("1,5", 0, 0, 0, 1, 1.0);
    }
  }
  require(std::setlocale(LC_NUMERIC, originalLocale.c_str()) != nullptr,
          "restore numeric locale");
#ifdef GTI_HAS_STD_FLOAT_FROM_CHARS
  const std::vector<std::string> inputs = {"",
                                           "-",
                                           "+1",
                                           " 1",
                                           ".",
                                           "1e+",
                                           "-0x1.8p2",
                                           "0x1.8p2",
                                           "nan",
                                           "-nan(123)",
                                           "NaN(payload_7)!",
                                           "nan(a-b)",
                                           "INFINITY!",
                                           "1.000000059604644775390625",
                                           "5e-324",
                                           "1e-99999",
                                           "1e99999",
                                           "0e999999",
                                           "1p-1074",
                                           "1p-1075",
                                           "1.00000000000008p-1075",
                                           "1.fffffffffffffp1023",
                                           "1.fffffffffffff8p1023",
                                           "1.fffffep127",
                                           "1.ffffffp127",
                                           "1p-149",
                                           "1p-150",
                                           "1.000001p-150",
                                           "0p999999"};
  for (const auto &input : inputs) {
    for (uint32_t format = 0; format < 4; ++format) {
      differential<float>(input, format);
      differential<double>(input, format);
    }
  }
  std::mt19937_64 random(410123);
  for (int index = 0; index < 30000; ++index) {
    std::string input = random() % 2 ? "-" : "";
    const unsigned base = random() % 2 ? 10 : 16;
    const auto digits = 1 + random() % 80;
    for (uint64_t digit = 0; digit < digits; ++digit) {
      input += "0123456789abcdef"[random() % base];
    }
    input.insert(input.size() - random() % digits, ".");
    input += base == 10 ? 'e' : 'p';
    input += std::to_string(int(random() % 2400) - 1200);
    input += '!';
    differential<float>(input, base == 10 ? 0 : 3);
    differential<double>(input, base == 10 ? 0 : 3);
  }
#endif
  require(std::fesetround(originalRounding) == 0, "restore rounding mode");
}
