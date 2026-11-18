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
  [[nodiscard]] std::size_t blockCount() const { return blocks.size(); }

  [[nodiscard]] bool isReachable(MirBlockId block) const {
    return block != 0 && block <= blocks.size() && blocks[block - 1].entry != 0;
  }

  // Returns zero for the entry block, unreachable blocks, and invalid IDs.
  [[nodiscard]] MirBlockId immediateDominator(MirBlockId block) const {
    return block == 0 || block > blocks.size() ? 0 : blocks[block - 1].parent;
  }

  [[nodiscard]] bool dominates(MirBlockId dominator, MirBlockId block) const;

  friend bool operator==(const MirDominanceInfo &,
                         const MirDominanceInfo &) = default;

private:
  friend std::optional<MirDominanceInfo>
  computeMirDominance(const MirBody &body);

  struct Block {
    MirBlockId parent = 0;
    // Inclusive subtree interval in dominator-tree preorder. Zero is reserved
    // for unreachable blocks. Both endpoints fit even at the CFG size limit.
    unsigned entry = 0;
    unsigned exit = 0;

    // Traversal order among siblings is not a dominance fact.
    bool operator==(const Block &other) const {
      return parent == other.parent && (entry != 0) == (other.entry != 0);
    }
  };
  std::vector<Block> blocks;
};

// Computes fresh dominance information for a structurally usable MIR CFG.
// No result is returned when block identities, the entry, or an edge target
// are invalid. The result never retains pointers into the body.
[[nodiscard]] std::optional<MirDominanceInfo>
computeMirDominance(const MirBody &body);

} // namespace lang
