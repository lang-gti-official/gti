#pragma once

// Test-only reviewed native extension profiles, not a production admission
// rule. No headers: preprocessing-only cross-target rejection needs no foreign
// SDK.
#if defined(__APPLE__) && defined(__MACH__) && defined(__aarch64__) &&         \
    defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__)
#if !defined(__clang__) ||                                                     \
    !((defined(__apple_build_version__) && __clang_major__ == 21) ||           \
      (!defined(__apple_build_version__) && __clang_major__ == 23))
#error GTI_TEST_WAIT_LANE_UNREVIEWED_COMPILER
#endif
#elif defined(__linux__) && defined(__x86_64__)
#if defined(__clang__)
#if __clang_major__ != 21 || defined(__apple_build_version__)
#error GTI_TEST_WAIT_LANE_UNREVIEWED_COMPILER
#endif
#elif !defined(__GNUC__) || __GNUC__ != 14 || defined(__INTEL_COMPILER) ||     \
    defined(__INTEL_LLVM_COMPILER)
#error GTI_TEST_WAIT_LANE_UNREVIEWED_COMPILER
#endif
#else
#error GTI_TEST_WAIT_LANE_UNREVIEWED_TARGET
#endif

#if __CHAR_BIT__ != 8 || __SIZEOF_POINTER__ != 8 || __SIZEOF_SIZE_T__ != 8
#error GTI_TEST_WAIT_LANE_UNREVIEWED_ABI
#endif
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error GTI_TEST_WAIT_LANE_UNREVIEWED_ENDIAN
#endif
