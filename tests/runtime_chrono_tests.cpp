#include "gti/chrono.h"

#include "chrono_internal.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

void expect(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "runtime chrono test failed: " << message << '\n';
    std::exit(1);
  }
}

void testCompositionBoundary() {
  using gti::runtime::detail::composeChronoNanoseconds;
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  constexpr auto minimum = std::numeric_limits<std::int64_t>::min();

  std::int64_t output = 91;
  expect(composeChronoNanoseconds(0, 0, &output) == GTI_CHRONO_OK_V1 &&
             output == 0,
         "zero is exactly representable");
  expect(composeChronoNanoseconds(-1, 999'999'999, &output) ==
                 GTI_CHRONO_OK_V1 &&
             output == -1,
         "negative values use floor seconds and a positive remainder");
  expect(composeChronoNanoseconds(9'223'372'036, 854'775'807, &output) ==
                 GTI_CHRONO_OK_V1 &&
             output == maximum,
         "the positive int64 boundary is accepted exactly");
  expect(composeChronoNanoseconds(-9'223'372'037, 145'224'192, &output) ==
                 GTI_CHRONO_OK_V1 &&
             output == minimum,
         "the negative int64 boundary is accepted exactly");

  output = 73;
  expect(composeChronoNanoseconds(9'223'372'036, 854'775'808, &output) ==
                 GTI_CHRONO_OUT_OF_RANGE_V1 &&
             output == 73,
         "positive overflow preserves the output");
  expect(composeChronoNanoseconds(-9'223'372'037, 145'224'191, &output) ==
                 GTI_CHRONO_OUT_OF_RANGE_V1 &&
             output == 73,
         "negative overflow preserves the output");
  expect(composeChronoNanoseconds(0, -1, &output) ==
                 GTI_CHRONO_OUT_OF_RANGE_V1 &&
             composeChronoNanoseconds(0, 1'000'000'000, &output) ==
                 GTI_CHRONO_OUT_OF_RANGE_V1 &&
             output == 73,
         "remainders must be normalized and failures preserve output");
  expect(composeChronoNanoseconds(0, 0, nullptr) == GTI_CHRONO_OUT_OF_RANGE_V1,
         "the internal boundary rejects a missing output");
}

void testHostClocks() {
  std::int64_t first = 0;
  std::int64_t second = 0;
  expect(gti_rt_chrono_steady_now_v1(&first) == GTI_CHRONO_OK_V1 &&
             gti_rt_chrono_steady_now_v1(&second) == GTI_CHRONO_OK_V1 &&
             second >= first,
         "successive steady readings are nondecreasing");

  std::int64_t wall = 0;
  expect(gti_rt_chrono_system_now_v1(&wall) == GTI_CHRONO_OK_V1,
         "the supported host wall clock is readable");
  expect(wall > 0, "the current wall clock is after the Unix epoch");

  expect(gti_rt_chrono_steady_now_v1(nullptr) == GTI_CHRONO_OUT_OF_RANGE_V1 &&
             gti_rt_chrono_system_now_v1(nullptr) == GTI_CHRONO_OUT_OF_RANGE_V1,
         "public clock entries reject a missing output");
}

} // namespace

int main() {
  testCompositionBoundary();
  testHostClocks();
  return 0;
}
