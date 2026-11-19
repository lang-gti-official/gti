#pragma once

#include "gti/llvm_admission.h"

#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>

namespace lang::native_llvm {
// Coverage entries returned by coverage(value) have static, unique identity.
// This index never determines publication order: the caller appends to its
// ordered use vector only on first insertion, and increments every occurrence.
class CoverageUseIndex final {
public:
  struct Key {
    const LlvmCoverageEntry *entry;
    std::optional<MirBodyAddress> body;
    bool required;
    bool executable;
    const LlvmCoverageEntry *shape;
    friend bool operator==(const Key &, const Key &) = default;
  };

  explicit CoverageUseIndex(bool validate) {
    if (validate)
      original.emplace();
  }

  std::pair<std::size_t, bool> insert(const Key &key, std::size_t next) {
    const auto [found, inserted] = indices.try_emplace(key, next);
    const std::pair<std::size_t, bool> result{found->second, inserted};
    if (original) {
      // Re-run the exact former string-keyed operation, including first-use
      // index assignment. Validate hits and misses without scanning use order.
      const auto [old, first] = original->try_emplace(
          OriginalKey{key.entry->family, key.entry->name, key.body.has_value(),
                      key.body ? key.body->kind : MirBodyKind::Module,
                      key.body ? key.body->owner : 0, key.required,
                      key.executable, key.shape},
          next);
      if (result != std::pair<std::size_t, bool>{old->second, first})
        throw std::logic_error("LLVM admission coverage index mismatch for " +
                               key.entry->key());
    }
    return result;
  }

private:
  struct Hash {
    std::size_t operator()(const Key &key) const {
      std::size_t result = std::hash<const LlvmCoverageEntry *>{}(key.entry);
      const auto mix = [&](std::size_t value) {
        result ^= value + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
      };
      mix(key.body.has_value());
      if (key.body) {
        mix(static_cast<std::size_t>(key.body->kind));
        mix(key.body->owner);
      }
      mix(key.required);
      mix(key.executable);
      mix(std::hash<const LlvmCoverageEntry *>{}(key.shape));
      return result;
    }
  };
  using OriginalKey =
      std::tuple<std::string_view, std::string_view, bool, MirBodyKind,
                 std::size_t, bool, bool, const LlvmCoverageEntry *>;
  std::unordered_map<Key, std::size_t, Hash> indices;
  std::optional<std::map<OriginalKey, std::size_t>> original;
  friend struct CoverageUseIndexTestAccess;
};
} // namespace lang::native_llvm
