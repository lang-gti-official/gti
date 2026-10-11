#include <cstdint>
#include <cstdio>
extern "C" void text_view_drop(std::int32_t value) {
  std::printf("%d\n", value);
}
