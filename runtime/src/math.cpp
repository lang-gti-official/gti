#include "gti/math.h"

#include <cmath>
#include <limits>

static_assert(std::numeric_limits<float>::is_iec559 &&
                  std::numeric_limits<float>::digits == 24 &&
                  sizeof(float) == 4,
              "GTI host mathematics requires IEEE-754 binary32");

// Keep the runtime seam scalar and independent of public GTI library names.
// The float overloads avoid an unintended binary64 calculation/narrowing pair.
extern "C" float gti_rt_math_floor_f32_v1(float value) {
  return std::floor(value);
}
extern "C" float gti_rt_math_ceil_f32_v1(float value) {
  return std::ceil(value);
}
extern "C" float gti_rt_math_round_f32_v1(float value) {
  return std::round(value);
}
extern "C" float gti_rt_math_trunc_f32_v1(float value) {
  return std::trunc(value);
}
extern "C" float gti_rt_math_sqrt_f32_v1(float value) {
  return std::sqrt(value);
}
extern "C" float gti_rt_math_cbrt_f32_v1(float value) {
  return std::cbrt(value);
}
extern "C" float gti_rt_math_pow_f32_v1(float base, float exponent) {
  return std::pow(base, exponent);
}
extern "C" float gti_rt_math_fmod_f32_v1(float numerator, float denominator) {
  return std::fmod(numerator, denominator);
}
extern "C" float gti_rt_math_exp_f32_v1(float value) { return std::exp(value); }
extern "C" float gti_rt_math_log_f32_v1(float value) { return std::log(value); }
extern "C" float gti_rt_math_log10_f32_v1(float value) {
  return std::log10(value);
}
extern "C" float gti_rt_math_sin_f32_v1(float value) { return std::sin(value); }
extern "C" float gti_rt_math_cos_f32_v1(float value) { return std::cos(value); }
extern "C" float gti_rt_math_tan_f32_v1(float value) { return std::tan(value); }
extern "C" float gti_rt_math_asin_f32_v1(float value) {
  return std::asin(value);
}
extern "C" float gti_rt_math_acos_f32_v1(float value) {
  return std::acos(value);
}
extern "C" float gti_rt_math_atan_f32_v1(float value) {
  return std::atan(value);
}
extern "C" float gti_rt_math_atan2_f32_v1(float y, float x) {
  return std::atan2(y, x);
}
