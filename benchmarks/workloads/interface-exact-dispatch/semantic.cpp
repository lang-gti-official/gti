#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

std::int32_t step(std::int32_t value, std::int32_t bias) {
#ifdef IDIOMATIC
  return (value * 17 + bias) % 65521;
#else
  std::int32_t product, sum;
  if (__builtin_mul_overflow(value, 17, &product) ||
      __builtin_add_overflow(product, bias, &sum))
    std::abort();
  return sum % 65521;
#endif
}
struct Update {
  virtual std::int32_t run(std::int32_t, std::int32_t) const = 0;
  virtual ~Update() = default;
};
struct Linear : Update {
  std::int32_t run(std::int32_t value, std::int32_t bias) const override {
    return step(value, bias);
  }
};
int main() {
  std::vector<std::int32_t> state(1024);
  for (std::size_t i = 0; i < state.size(); ++i)
    state[i] = std::int32_t(i);
  Linear action;
  const Update &selected = action;
  for (std::int32_t tick = 0; tick < 120000; ++tick) {
    std::int32_t bias = tick % 127;
    for (std::size_t i = 0; i < state.size(); ++i) {
#ifndef IDIOMATIC
      if (i >= state.size())
        std::abort();
#endif
      state[i] = selected.run(state[i], bias);
    }
  }
  std::uint64_t digest = 0;
  for (auto value : state) {
    if (value < 0 || value >= 65521)
      return 1;
#ifdef FULL_STATE
    std::printf("%d\n", value);
#endif
    digest = (digest * 131 + std::uint64_t(value)) % 1000000007;
  }
  if (digest != 480748635)
    return 1;
#ifndef FULL_STATE
  std::printf("GTI-BENCH-1 digest:%016llx work-units:122880000\n",
              static_cast<unsigned long long>(digest));
#endif
}
