#pragma once

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

namespace lang::optimization_detail {

// Analysis-owned proof IDs are maintained as sorted unique vectors. These
// helpers change their construction cost, not their canonical serialization or
// meaning. They must not be used for uncanonicalized external evidence.
template <typename T> void insertEvidenceId(std::vector<T> &ids, T id) {
  if (id == 0) {
    return;
  }
  const auto position = std::lower_bound(ids.begin(), ids.end(), id);
  if (position == ids.end() || *position != id) {
    ids.insert(position, id);
  }
}

template <typename T>
void mergeEvidenceIds(std::vector<T> &target, const std::vector<T> &source) {
  // Joins repeatedly see identical/subset evidence at the fixed point. Do not
  // allocate, append duplicates or sort again in that case. This also handles
  // aliasing (merging a vector into itself) without invalidating source.
  if (source.empty() || std::includes(target.begin(), target.end(),
                                      source.begin(), source.end())) {
    return;
  }
  if (target.empty()) {
    target = source;
    return;
  }
  std::vector<T> merged;
  merged.reserve(target.size() + source.size());
  std::set_union(target.begin(), target.end(), source.begin(), source.end(),
                 std::back_inserter(merged));
  target = std::move(merged);
}

} // namespace lang::optimization_detail
