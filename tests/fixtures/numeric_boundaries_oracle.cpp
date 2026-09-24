#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <type_traits>

namespace {
std::uint64_t bits(std::uint64_t i) {
  // Fixed test data only, not a production hashing implementation.
  i += UINT64_C(0x9e3779b97f4a7c15);
  i = (i ^ (i >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  i = (i ^ (i >> 27)) * UINT64_C(0x94d049bb133111eb);
  return i ^ (i >> 31);
}
template <class T> T input(std::int32_t i) {
  using U = std::make_unsigned_t<T>;
  if constexpr (sizeof(T) == 1) {
    return std::bit_cast<T>(static_cast<U>(i));
  } else {
    constexpr T lo = std::numeric_limits<T>::min();
    constexpr T hi = std::numeric_limits<T>::max();
    constexpr std::array<T, 12> edges{
        lo,   T(lo + 1), T(lo + 2), hi,   T(hi - 1), T(hi - 2),
        T(0), T(1),      T(2),      T(3), T(hi / 2), T(hi / 2 + 1)};
    if (i < static_cast<std::int32_t>(edges.size()))
      return edges[i];
    return std::bit_cast<T>(
        static_cast<U>(bits(static_cast<std::uint64_t>(i))));
  }
}
template <class T> auto magnitude(T value) {
  using U = std::make_unsigned_t<T>;
  U raw = static_cast<U>(value);
  if constexpr (std::is_signed_v<T>) {
    if (value < 0)
      return U(U(0) - raw);
  }
  return raw;
}
template <class T> auto gcd(T a, T b) {
  return std::gcd(magnitude(a), magnitude(b));
}
template <class T> bool gcdFits(T a, T b) {
  return gcd(a, b) <=
         static_cast<std::make_unsigned_t<T>>(std::numeric_limits<T>::max());
}
template <class T> bool lcmFits(T a, T b) {
  auto x = magnitude(a), y = magnitude(b);
  if (x == 0 || y == 0)
    return true;
  auto q = x / std::gcd(x, y);
  return q <=
         static_cast<std::make_unsigned_t<T>>(std::numeric_limits<T>::max()) /
             y;
}
template <class T> T lcm(T a, T b) {
  auto x = magnitude(a), y = magnitude(b);
  if (x == 0 || y == 0)
    return 0;
  return static_cast<T>((x / std::gcd(x, y)) * y);
}
template <class T, class U> T floatingInput(std::int32_t i) {
  constexpr T tiny = std::numeric_limits<T>::denorm_min();
  constexpr T small = std::numeric_limits<T>::min();
  constexpr T huge = std::numeric_limits<T>::max();
  constexpr T infinity = std::numeric_limits<T>::infinity();
  constexpr std::array<T, 22> edges{
      T(0),           T(-0.0),     tiny,
      -tiny,          T(tiny * 2), T(tiny * 3),
      small,          -small,      huge,
      -huge,          T(huge / 2), T(-huge / 2),
      T(1),           T(-1),       T(2),
      T(-2),          T(0.5),      T(-0.5),
      infinity,       -infinity,   std::numeric_limits<T>::quiet_NaN(),
      T(small + tiny)};
  if (i < static_cast<std::int32_t>(edges.size()))
    return edges[i];
  return std::bit_cast<T>(static_cast<U>(bits(static_cast<std::uint64_t>(i))));
}
template <class T, class U> bool midpointMatches(T a, T b, T actual) {
  const T expected = std::midpoint(a, b);
  if (std::isnan(expected))
    return std::isnan(actual);
  return std::bit_cast<U>(actual) == std::bit_cast<U>(expected);
}
} // namespace
extern "C" std::int8_t numeric_input_int8_t(std::int32_t i) {
  return input<std::int8_t>(i);
}
extern "C" std::int8_t numeric_gcd_int8_t(std::int8_t a, std::int8_t b) {
  return static_cast<std::int8_t>(gcd(a, b));
}
extern "C" std::int8_t numeric_lcm_int8_t(std::int8_t a, std::int8_t b) {
  return lcm(a, b);
}
extern "C" std::int32_t numeric_gcd_fits_int8_t(std::int8_t a, std::int8_t b) {
  return gcdFits(a, b);
}
extern "C" std::int32_t numeric_lcm_fits_int8_t(std::int8_t a, std::int8_t b) {
  return lcmFits(a, b);
}
extern "C" std::int8_t numeric_midpoint_int8_t(std::int8_t a, std::int8_t b) {
  return std::midpoint(a, b);
}
extern "C" std::uint8_t numeric_input_uint8_t(std::int32_t i) {
  return input<std::uint8_t>(i);
}
extern "C" std::uint8_t numeric_gcd_uint8_t(std::uint8_t a, std::uint8_t b) {
  return static_cast<std::uint8_t>(gcd(a, b));
}
extern "C" std::uint8_t numeric_lcm_uint8_t(std::uint8_t a, std::uint8_t b) {
  return lcm(a, b);
}
extern "C" std::int32_t numeric_gcd_fits_uint8_t(std::uint8_t a,
                                                 std::uint8_t b) {
  return gcdFits(a, b);
}
extern "C" std::int32_t numeric_lcm_fits_uint8_t(std::uint8_t a,
                                                 std::uint8_t b) {
  return lcmFits(a, b);
}
extern "C" std::uint8_t numeric_midpoint_uint8_t(std::uint8_t a,
                                                 std::uint8_t b) {
  return std::midpoint(a, b);
}
extern "C" std::int16_t numeric_input_int16_t(std::int32_t i) {
  return input<std::int16_t>(i);
}
extern "C" std::int16_t numeric_gcd_int16_t(std::int16_t a, std::int16_t b) {
  return static_cast<std::int16_t>(gcd(a, b));
}
extern "C" std::int16_t numeric_lcm_int16_t(std::int16_t a, std::int16_t b) {
  return lcm(a, b);
}
extern "C" std::int32_t numeric_gcd_fits_int16_t(std::int16_t a,
                                                 std::int16_t b) {
  return gcdFits(a, b);
}
extern "C" std::int32_t numeric_lcm_fits_int16_t(std::int16_t a,
                                                 std::int16_t b) {
  return lcmFits(a, b);
}
extern "C" std::int16_t numeric_midpoint_int16_t(std::int16_t a,
                                                 std::int16_t b) {
  return std::midpoint(a, b);
}
extern "C" std::uint16_t numeric_input_uint16_t(std::int32_t i) {
  return input<std::uint16_t>(i);
}
extern "C" std::uint16_t numeric_gcd_uint16_t(std::uint16_t a,
                                              std::uint16_t b) {
  return static_cast<std::uint16_t>(gcd(a, b));
}
extern "C" std::uint16_t numeric_lcm_uint16_t(std::uint16_t a,
                                              std::uint16_t b) {
  return lcm(a, b);
}
extern "C" std::int32_t numeric_gcd_fits_uint16_t(std::uint16_t a,
                                                  std::uint16_t b) {
  return gcdFits(a, b);
}
extern "C" std::int32_t numeric_lcm_fits_uint16_t(std::uint16_t a,
                                                  std::uint16_t b) {
  return lcmFits(a, b);
}
extern "C" std::uint16_t numeric_midpoint_uint16_t(std::uint16_t a,
                                                   std::uint16_t b) {
  return std::midpoint(a, b);
}
extern "C" std::int32_t numeric_input_int32_t(std::int32_t i) {
  return input<std::int32_t>(i);
}
extern "C" std::int32_t numeric_gcd_int32_t(std::int32_t a, std::int32_t b) {
  return static_cast<std::int32_t>(gcd(a, b));
}
extern "C" std::int32_t numeric_lcm_int32_t(std::int32_t a, std::int32_t b) {
  return lcm(a, b);
}
extern "C" std::int32_t numeric_gcd_fits_int32_t(std::int32_t a,
                                                 std::int32_t b) {
  return gcdFits(a, b);
}
extern "C" std::int32_t numeric_lcm_fits_int32_t(std::int32_t a,
                                                 std::int32_t b) {
  return lcmFits(a, b);
}
extern "C" std::int32_t numeric_midpoint_int32_t(std::int32_t a,
                                                 std::int32_t b) {
  return std::midpoint(a, b);
}
extern "C" std::uint32_t numeric_input_uint32_t(std::int32_t i) {
  return input<std::uint32_t>(i);
}
extern "C" std::uint32_t numeric_gcd_uint32_t(std::uint32_t a,
                                              std::uint32_t b) {
  return static_cast<std::uint32_t>(gcd(a, b));
}
extern "C" std::uint32_t numeric_lcm_uint32_t(std::uint32_t a,
                                              std::uint32_t b) {
  return lcm(a, b);
}
extern "C" std::int32_t numeric_gcd_fits_uint32_t(std::uint32_t a,
                                                  std::uint32_t b) {
  return gcdFits(a, b);
}
extern "C" std::int32_t numeric_lcm_fits_uint32_t(std::uint32_t a,
                                                  std::uint32_t b) {
  return lcmFits(a, b);
}
extern "C" std::uint32_t numeric_midpoint_uint32_t(std::uint32_t a,
                                                   std::uint32_t b) {
  return std::midpoint(a, b);
}
extern "C" std::int64_t numeric_input_int64_t(std::int32_t i) {
  return input<std::int64_t>(i);
}
extern "C" std::int64_t numeric_gcd_int64_t(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(gcd(a, b));
}
extern "C" std::int64_t numeric_lcm_int64_t(std::int64_t a, std::int64_t b) {
  return lcm(a, b);
}
extern "C" std::int32_t numeric_gcd_fits_int64_t(std::int64_t a,
                                                 std::int64_t b) {
  return gcdFits(a, b);
}
extern "C" std::int32_t numeric_lcm_fits_int64_t(std::int64_t a,
                                                 std::int64_t b) {
  return lcmFits(a, b);
}
extern "C" std::int64_t numeric_midpoint_int64_t(std::int64_t a,
                                                 std::int64_t b) {
  return std::midpoint(a, b);
}
extern "C" std::uint64_t numeric_input_uint64_t(std::int32_t i) {
  return input<std::uint64_t>(i);
}
extern "C" std::uint64_t numeric_gcd_uint64_t(std::uint64_t a,
                                              std::uint64_t b) {
  return static_cast<std::uint64_t>(gcd(a, b));
}
extern "C" std::uint64_t numeric_lcm_uint64_t(std::uint64_t a,
                                              std::uint64_t b) {
  return lcm(a, b);
}
extern "C" std::int32_t numeric_gcd_fits_uint64_t(std::uint64_t a,
                                                  std::uint64_t b) {
  return gcdFits(a, b);
}
extern "C" std::int32_t numeric_lcm_fits_uint64_t(std::uint64_t a,
                                                  std::uint64_t b) {
  return lcmFits(a, b);
}
extern "C" std::uint64_t numeric_midpoint_uint64_t(std::uint64_t a,
                                                   std::uint64_t b) {
  return std::midpoint(a, b);
}
extern "C" float numeric_input_float(std::int32_t i) {
  return floatingInput<float, std::uint32_t>(i);
}
extern "C" double numeric_input_double(std::int32_t i) {
  return floatingInput<double, std::uint64_t>(i);
}
extern "C" std::int32_t numeric_midpoint_float(float a, float b, float actual) {
  return midpointMatches<float, std::uint32_t>(a, b, actual);
}
extern "C" std::int32_t numeric_midpoint_double(double a, double b,
                                                double actual) {
  return midpointMatches<double, std::uint64_t>(a, b, actual);
}
