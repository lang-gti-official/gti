#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <vector>
int32_t step(int32_t value, int32_t bias) {
#ifdef IDIOMATIC
  return (value * 17 + bias) % 65521;
#else
  int32_t product, sum;
  if (__builtin_mul_overflow(value, 17, &product) ||
      __builtin_add_overflow(product, bias, &sum))
    std::abort();
  return sum % 65521;
#endif
}
struct Update {
  virtual int32_t run(int32_t, int32_t) const = 0;
  virtual ~Update() = default;
};
struct Linear : Update {
  int32_t run(int32_t v, int32_t b) const override { return step(v, b); }
};
int32_t dispatch(const Update &action, int32_t v, int32_t b) {
  return action.run(v, b);
}
extern "C" int32_t native_step(int32_t, int32_t);
int32_t owned_step(int32_t v, int32_t b) {
  int32_t result = step(v, b);
#ifdef IDIOMATIC
  // Both languages permit process-allocation elision (ADR 022 revision).
  auto owner = std::make_unique<int32_t>(result);
  return *owner;
#else
  auto owner = std::unique_ptr<int32_t>(new (std::nothrow) int32_t(result));
  if (!owner)
    std::abort();
  return *owner;
#endif
}
int main() {
  std::vector<int32_t> state(1024), bias(1024);
  for (std::size_t i = 0; i < state.size(); ++i) {
    state[i] = int32_t(i);
    bias[i] = int32_t(i % 127);
  }
  std::function<int32_t(int32_t, int32_t)> action = [](int32_t v, int32_t b) {
    return step(v, b);
  };
  Linear linear;
#ifdef SOA
  constexpr int ticks = 240000;
#else
  constexpr int ticks = 120000;
#endif
  for (int32_t tick = 0; tick < ticks; ++tick) {
    int32_t b = tick % 127;
    for (std::size_t i = 0; i < state.size(); ++i) {
#ifndef IDIOMATIC
      if (i >= state.size())
        std::abort();
#endif
#if defined(CALLBACK)
      state[i] = action(state[i], b);
#elif defined(INTERFACE)
      state[i] = dispatch(linear, state[i], b);
#elif defined(OWNER)
      state[i] = owned_step(state[i], b);
#elif defined(SOA)
#ifndef IDIOMATIC
      if (i >= bias.size())
        std::abort();
#endif
      state[i] = step(state[i], bias[i]);
#elif defined(CROSS_UNIT)
      state[i] = native_step(state[i], b);
#else
      state[i] = step(state[i], b);
#endif
    }
  }
  for (auto value : state)
    std::printf("%d\n", value);
}
