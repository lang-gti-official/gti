#include <cstdio>

extern "C" void user_dual_range_event(int identity) {
  std::printf("E%d\n", identity);
  std::fflush(stdout);
}
