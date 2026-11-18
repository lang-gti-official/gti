#pragma once

#include "gti/semantic_core.h"
#include <cstddef>

namespace lang::native_llvm {
// A structural bucket selector shared by hierarchy outcomes and admission
// indexes. It deliberately omits some fields; full equality decides every hit.
struct TypeBucketHash {
  std::size_t operator()(const SemanticType &type) const {
    std::size_t hash = static_cast<std::size_t>(type.kind);
    const auto mix = [&](std::size_t value) {
      hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    };
    mix(type.classId);
    mix(type.enumId);
    mix(type.lambdaId);
    mix(type.arrayLength);
    mix(type.arguments.size());
    for (const auto &argument : type.arguments)
      mix((*this)(argument));
    return hash;
  }
};
} // namespace lang::native_llvm
