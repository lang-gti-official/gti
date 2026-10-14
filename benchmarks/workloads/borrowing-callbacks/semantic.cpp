#include <cstdint>
#include <cstdio>
#include <cstdlib>

struct MutableView {
  void *object;
  void (*invoke)(void *, std::uint64_t);
  void operator()(std::uint64_t bias) { invoke(object, bias); }
};

int main() {
  std::uint64_t state = 3;
  auto update = [&state](std::uint64_t bias) {
    std::uint64_t product, sum;
    if (__builtin_mul_overflow(state, std::uint64_t(17), &product) ||
        __builtin_add_overflow(product, bias, &sum))
      std::abort();
    state = sum % std::uint64_t(65521);
  };
  MutableView view{&update, [](void *object, std::uint64_t bias) {
                     (*static_cast<decltype(update) *>(object))(bias);
                   }};
  for (std::uint64_t tick = 0; tick < 40000000; ++tick)
    view(tick % std::uint64_t(127));
  std::printf("GTI-BENCH-1 digest:%016llx work-units:40000000\n",
              static_cast<unsigned long long>(state));
  return state == 0xa4e8 ? 0 : 1;
}
