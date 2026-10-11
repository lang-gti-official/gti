#include <cstdint>
#include <cstdio>
namespace {
std::int32_t value_dereferences = 0;
}
extern "C" void text_view_drop(std::int32_t value) {
  std::printf("%d\n", value);
}
extern "C" void text_view_note_dereference() { ++value_dereferences; }
extern "C" std::int32_t text_view_dereferences() { return value_dereferences; }
