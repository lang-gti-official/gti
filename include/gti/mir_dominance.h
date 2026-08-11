#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace lang {

// Snapshot-scoped dominance facts for one MIR body. Results are expressed
// only in GTI block identities; the implementation may use a private CFG
// representation while computing them.
class MirDominanceInfo {
public:
  [[nodiscard]] std::size_t blockCount() const {
    return immediateDominators.size();
  }

  [[nodiscard]] bool isReachable(MirBlockId block) const {
    return block != 0 && block <= reachableBlocks.size() &&
           reachableBlocks[block - 1];
  }

  // Returns zero for the entry block, unreachable blocks, and invalid IDs.
  [[nodiscard]] MirBlockId immediateDominator(MirBlockId block) const {
    return block == 0 || block > immediateDominators.size()
               ? 0
               : immediateDominators[block - 1];
  }

  [[nodiscard]] bool dominates(MirBlockId dominator, MirBlockId block) const {
    if (!isReachable(dominator) || !isReachable(block)) {
      return false;
    }
    if (dominator == block) {
      return true;
    }

    MirBlockId current = immediateDominator(block);
    for (std::size_t depth = 0;
         current != 0 && depth < immediateDominators.size(); ++depth) {
      if (current == dominator) {
        return true;
      }
      current = immediateDominator(current);
    }
    return false;
  }

  friend bool operator==(const MirDominanceInfo &,
                         const MirDominanceInfo &) = default;

private:
  friend std::optional<MirDominanceInfo>
  computeMirDominance(const MirBody &body);

  std::vector<MirBlockId> immediateDominators;
  std::vector<bool> reachableBlocks;
};

// Computes fresh dominance information for a structurally usable MIR CFG.
// No result is returned when block identities, the entry, or an edge target
// are invalid. The result never retains pointers into the body.
[[nodiscard]] std::optional<MirDominanceInfo>
computeMirDominance(const MirBody &body);

} // namespace lang
