#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Binary32 host mathematics, version one. These operations return IEEE values:
// domain errors produce NaN, poles/overflow produce infinity, and underflow
// produces a subnormal or signed zero. They never raise a GTI defined failure.
// Callers must preserve the runtime's round-to-nearest, gradual-underflow
// environment. errno and floating exception flags are not part of this ABI.
float gti_rt_math_floor_f32_v1(float value);
float gti_rt_math_ceil_f32_v1(float value);
float gti_rt_math_round_f32_v1(float value);
float gti_rt_math_trunc_f32_v1(float value);
float gti_rt_math_sqrt_f32_v1(float value);
float gti_rt_math_cbrt_f32_v1(float value);
float gti_rt_math_pow_f32_v1(float base, float exponent);
float gti_rt_math_fmod_f32_v1(float numerator, float denominator);
float gti_rt_math_exp_f32_v1(float value);
float gti_rt_math_log_f32_v1(float value);
float gti_rt_math_log10_f32_v1(float value);
float gti_rt_math_sin_f32_v1(float value);
float gti_rt_math_cos_f32_v1(float value);
float gti_rt_math_tan_f32_v1(float value);
float gti_rt_math_asin_f32_v1(float value);
float gti_rt_math_acos_f32_v1(float value);
float gti_rt_math_atan_f32_v1(float value);
float gti_rt_math_atan2_f32_v1(float y, float x);

#ifdef __cplusplus
}
#endif
