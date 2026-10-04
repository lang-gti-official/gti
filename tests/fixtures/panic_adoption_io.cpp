#include <cstdio>

// Test-only host setup: expose submission errors immediately rather than
// deferring small buffered writes until process exit.
extern "C" void gti_test_unbuffer_stdout() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
}
