#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::int32_t add_i32(std::int32_t left, std::int32_t right) {
#ifndef IDIOMATIC
  const auto wide = std::int64_t(left) + std::int64_t(right);
  if (wide < std::numeric_limits<std::int32_t>::min() ||
      wide > std::numeric_limits<std::int32_t>::max())
    std::abort();
#endif
  return left + right;
}

std::int32_t multiply_i32(std::int32_t left, std::int32_t right) {
#ifndef IDIOMATIC
  const auto wide = std::int64_t(left) * std::int64_t(right);
  if (wide < std::numeric_limits<std::int32_t>::min() ||
      wide > std::numeric_limits<std::int32_t>::max())
    std::abort();
#endif
  return left * right;
}

template <class T> T add_unsigned(T left, T right) {
#ifndef IDIOMATIC
  if (left > std::numeric_limits<T>::max() - right)
    std::abort();
#endif
  return left + right;
}

std::size_t index_of(std::int32_t value) {
#ifndef IDIOMATIC
  if (value < 0)
    std::abort();
#endif
  return static_cast<std::size_t>(value);
}

char at(std::string_view text, std::size_t index) {
#ifndef IDIOMATIC
  if (index >= text.size())
    std::abort();
#endif
  return text[index];
}

} // namespace

int main() {
  // Fixed inputs do not fail. The checked twin preserves the arithmetic and
  // indexing preconditions, not GTI's failure-report ABI or capacity policy.
  try {
    constexpr std::string_view alphabet = "abcdefghijklmnopqrstuvwxyz";
    std::uint64_t checksum = 0;
    for (std::int32_t round = 0; round < 400; round = add_i32(round, 1)) {
      std::string text;
      for (std::int32_t i = 0; i < 50000; i = add_i32(i, 1)) {
        text.push_back(
            at(alphabet, index_of(add_i32(multiply_i32(i, 7), round) % 26)));
      }
#ifdef FULL_STATE
      std::puts(text.c_str());
#endif
      std::size_t position = text.find('q');
      while (position != std::string::npos) {
#ifdef FULL_STATE
        std::printf("%zu\n", position);
#endif
        checksum = add_unsigned(checksum, static_cast<std::uint64_t>(position));
        position = text.find('q', add_unsigned(position, std::size_t{1}));
      }
      checksum =
          add_unsigned(checksum, static_cast<std::uint64_t>(text.size()));
    }
    if (checksum != 19250373078ULL)
      return 1;
    std::printf("GTI-BENCH-1 digest:%016llx work-units:20000000\n",
                static_cast<unsigned long long>(checksum));
  } catch (const std::bad_alloc &) {
    std::abort();
  } catch (const std::length_error &) {
    std::abort();
  }
}
