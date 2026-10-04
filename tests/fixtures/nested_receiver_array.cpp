#include <cstdio>
#include <cstdlib>

extern "C" {
struct RawMatrix {
  float values[9];
};
RawMatrix gti_test_matrix() { return {{1, 2, 3, 4, 5, 6, 7, 8, 9}}; }
int gti_test_mode() {
  const char *mode = std::getenv("GTI_TEST_ARRAY_MODE");
  return mode ? std::atoi(mode) : 0;
}
void gti_test_drop(int identity) { std::printf("%d\n", identity); }
}
