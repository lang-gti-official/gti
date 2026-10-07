#include <cstdint>

struct Scene {
  int value;
};
struct Mesh {
  int value;
};
static Scene source{17};
static Mesh result{23};
extern "C" const Scene *scene() { return &source; }
extern "C" const Mesh *mesh(const Scene *value, std::uint32_t index) {
  return value == &source && index == 0 ? &result : nullptr;
}
extern "C" std::int32_t selected(const Mesh *value) { return value == &result; }
