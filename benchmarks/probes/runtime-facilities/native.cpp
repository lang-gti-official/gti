#include <cstdint>
#include <cstdlib>
extern "C" std::int32_t native_step(std::int32_t value, std::int32_t bias) {
  std::int32_t product, sum;
  if (__builtin_mul_overflow(value, 17, &product) ||
      __builtin_add_overflow(product, bias, &sum))
    std::abort();
  return sum % 65521;
}
