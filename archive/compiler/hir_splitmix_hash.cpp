// ARCHIVED: excluded from the GTI build. See archive/compiler/README.md.

#include <cstddef>
#include <cstdint>

namespace lang {
namespace {

using HashCode = std::uint64_t;

[[nodiscard]] HashCode combine(std::uint64_t seed, std::uint64_t value) {
  // splitmix64-style mixing.
  std::uint64_t mixed =
      seed ^ (value + 0x9E3779B97F4A7C15ULL + (seed << 6U) + (seed >> 2U));
  mixed ^= mixed >> 30U;
  mixed *= 0xBF58476D1CE4E5B9ULL;
  mixed ^= mixed >> 27U;
  return mixed;
}

[[nodiscard]] std::size_t finalize(HashCode code) {
  return static_cast<std::size_t>(code);
}

} // namespace
} // namespace lang
