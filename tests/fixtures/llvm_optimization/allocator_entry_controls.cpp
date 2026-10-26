#include <cstdlib>
#include <cstring>

extern "C" void gti_cost_begin() noexcept;
extern "C" int gti_cost_end(unsigned long long, unsigned long long) noexcept;

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const bool positive = std::strcmp(argv[1], "positive") == 0;
  if (!positive && std::strcmp(argv[1], "empty") != 0)
    return 2;
  gti_cost_begin();
  if (positive) {
    void *first = std::malloc(8);
    if (!first)
      return 3;
    void *grown = std::realloc(first, 16);
    if (!grown) {
      std::free(first);
      return 4;
    }
    std::free(grown);
    void *zeroed = std::calloc(2, 8);
    if (!zeroed)
      return 5;
    std::free(zeroed);
    void *aligned = nullptr;
    if (posix_memalign(&aligned, 256, 257) != 0 || !aligned)
      return 6;
    std::free(aligned);
  }
  return gti_cost_end(positive ? 1 : 0, 0);
}
