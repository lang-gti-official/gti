#pragma once

#include <cstddef>

namespace lang {

// Compilation resource policy shared by concrete discovery and prerequisite
// semantic body queries. Exact recursive instances do not consume another
// discovery step; these are not a termination proof or a cache capacity.
struct GenericInstantiationLimits {
  static constexpr std::size_t maximumDepth = 64;
  static constexpr std::size_t maximumDistinctInstances = 16384;
};

} // namespace lang
