#pragma once

#include "type_hash.h"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace lang::native_llvm {
// One admission plan owns each table. Callers establish a whole, path-free
// query before entering it; the compute callback is also the uncached oracle.
// Failed computations never populate the table. Full equality resolves hash
// collisions, and validation checks every hit before returning its answer.
template <class Key, class Answer, class Hash> class AdmissionMemo final {
public:
  explicit AdmissionMemo(const char *name) : name(name) {}

  template <class Compute>
  const Answer &query(const Key &key, const SemanticType &type, bool validate,
                      std::size_t *hits, Compute &&compute) const {
    if (const auto found = answers.find(key); found != answers.end()) {
      const Answer &answer = found->second;
      if (hits)
        ++*hits;
      if (validate && answer != compute())
        throw std::logic_error(
            std::string("LLVM admission memo mismatch for ") + name +
            " (kind " + std::to_string(type.kind) + ", class " +
            std::to_string(type.classId) + ", enum " +
            std::to_string(type.enumId) + ")");
      return answer;
    }
    Answer answer = compute();
    return answers.emplace(key, std::move(answer)).first->second;
  }

private:
  const char *name;
  mutable std::unordered_map<Key, Answer, Hash> answers;
  friend struct AdmissionMemoTestAccess;
};
} // namespace lang::native_llvm
