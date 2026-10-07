#include <cstdint>
#include <cstdio>
#include <cstdlib>

static int index_calls;
extern "C" int issue_mode() {
  const char *mode = std::getenv("GTI_ISSUE_MODE");
  return mode ? std::atoi(mode) : 0;
}
extern "C" void issue_trace(int value) { std::printf("%d\n", value); }
extern "C" std::uint64_t issue_index() {
  ++index_calls;
  return 1;
}
extern "C" int issue_index_calls() { return index_calls; }
