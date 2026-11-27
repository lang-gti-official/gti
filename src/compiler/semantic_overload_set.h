#pragma once

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace lang {

// Registration and substitution own writable candidates. Once the catalog
// seals, read-only snapshots share its signatures. A copy of a writable set
// still has vector value semantics, including during an outstanding edit.
template <typename Candidate> class SemanticOverloadSet {
  using List = std::vector<Candidate>;
  List owned;
  std::shared_ptr<const List> frozen;

  const List &read() const { return frozen ? *frozen : owned; }

public:
  SemanticOverloadSet() = default;
  SemanticOverloadSet(Candidate candidate) {
    owned.emplace_back(std::move(candidate));
  }
  bool empty() const { return read().empty(); }
  std::size_t size() const { return read().size(); }
  auto begin() const { return read().begin(); }
  auto end() const { return read().end(); }
  const Candidate &front() const { return read().front(); }
  const Candidate &operator[](std::size_t index) const { return read()[index]; }

  List &edit() {
    if (frozen) {
      owned = *frozen;
      frozen.reset();
    }
    return owned;
  }

  // Whether freeze() would change this set.
  bool freezable() const { return !owned.empty(); }

  // All writers have finished with this set at the sealing boundary.
  void freeze() {
    if (!owned.empty())
      frozen = std::make_shared<const List>(std::move(owned));
  }
};

} // namespace lang
