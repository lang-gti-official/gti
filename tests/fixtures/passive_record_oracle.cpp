#include <cstdio>
#include <cstdlib>

extern "C" void gti_test_trace(int value) { std::printf("%d\n", value); }
extern "C" int gti_test_zero() { return 0; }
extern "C" int gti_test_mode() {
  const char *mode = std::getenv("GTI_TEST_RECORD_MODE");
  return mode ? std::atoi(mode) : 0;
}
