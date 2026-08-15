#include "gti/mir_dominance.h"

#include "llvm/ADT/GraphTraits.h"
#include "llvm/Support/GenericDomTree.h"
#include "llvm/Support/GenericDomTreeConstruction.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace lang::mir_dominance_detail {

struct CfgSnapshot;

struct CfgNode {
  MirBlockId id = 0;
  CfgSnapshot *parent = nullptr;
  std::vector<CfgNode *> successors;
  std::vector<CfgNode *> predecessors;

  [[nodiscard]] CfgSnapshot *getParent() { return parent; }
  [[nodiscard]] const CfgSnapshot *getParent() const { return parent; }

  void printAsOperand(llvm::raw_ostream &output, bool) const {
    output << "bb" << id;
  }
};

struct CfgSnapshot {
  MirBlockId entry = 0;
  std::vector<CfgNode> nodes;
  std::vector<CfgNode *> nodePointers;

  [[nodiscard]] CfgNode &front() { return nodes[entry - 1]; }
  [[nodiscard]] const CfgNode &front() const { return nodes[entry - 1]; }
};

} // namespace lang::mir_dominance_detail

namespace llvm {

template <> struct GraphTraits<lang::mir_dominance_detail::CfgNode *> {
  using NodeRef = lang::mir_dominance_detail::CfgNode *;
  using ChildIteratorType = std::vector<NodeRef>::const_iterator;

  static NodeRef getEntryNode(NodeRef node) { return node; }

  static ChildIteratorType child_begin(NodeRef node) {
    return node->successors.begin();
  }

  static ChildIteratorType child_end(NodeRef node) {
    return node->successors.end();
  }

  static unsigned getNumber(NodeRef node) {
    return static_cast<unsigned>(node->id - 1);
  }
};

template <> struct GraphTraits<const lang::mir_dominance_detail::CfgNode *> {
  using NodeRef = const lang::mir_dominance_detail::CfgNode *;
  using ChildIteratorType =
      std::vector<lang::mir_dominance_detail::CfgNode *>::const_iterator;

  static NodeRef getEntryNode(NodeRef node) { return node; }

  static ChildIteratorType child_begin(NodeRef node) {
    return node->successors.begin();
  }

  static ChildIteratorType child_end(NodeRef node) {
    return node->successors.end();
  }

  static unsigned getNumber(NodeRef node) {
    return static_cast<unsigned>(node->id - 1);
  }
};

template <> struct GraphTraits<Inverse<lang::mir_dominance_detail::CfgNode *>> {
  using NodeRef = lang::mir_dominance_detail::CfgNode *;
  using ChildIteratorType = std::vector<NodeRef>::const_iterator;

  static ChildIteratorType child_begin(NodeRef node) {
    return node->predecessors.begin();
  }

  static ChildIteratorType child_end(NodeRef node) {
    return node->predecessors.end();
  }
};

template <>
struct GraphTraits<Inverse<const lang::mir_dominance_detail::CfgNode *>> {
  using NodeRef = const lang::mir_dominance_detail::CfgNode *;
  using ChildIteratorType =
      std::vector<lang::mir_dominance_detail::CfgNode *>::const_iterator;

  static ChildIteratorType child_begin(NodeRef node) {
    return node->predecessors.begin();
  }

  static ChildIteratorType child_end(NodeRef node) {
    return node->predecessors.end();
  }
};

template <> struct GraphTraits<lang::mir_dominance_detail::CfgSnapshot *> {
  using NodeRef = lang::mir_dominance_detail::CfgNode *;
  using nodes_iterator = std::vector<NodeRef>::const_iterator;

  static NodeRef
  getEntryNode(lang::mir_dominance_detail::CfgSnapshot *snapshot) {
    return &snapshot->front();
  }

  static unsigned
  getMaxNumber(lang::mir_dominance_detail::CfgSnapshot *snapshot) {
    return static_cast<unsigned>(snapshot->nodes.size());
  }

  static unsigned getNumberEpoch(lang::mir_dominance_detail::CfgSnapshot *) {
    return 0;
  }

  static nodes_iterator
  nodes_begin(lang::mir_dominance_detail::CfgSnapshot *snapshot) {
    return snapshot->nodePointers.begin();
  }

  static nodes_iterator
  nodes_end(lang::mir_dominance_detail::CfgSnapshot *snapshot) {
    return snapshot->nodePointers.end();
  }
};

template <>
struct GraphTraits<const lang::mir_dominance_detail::CfgSnapshot *> {
  using NodeRef = const lang::mir_dominance_detail::CfgNode *;
  using nodes_iterator =
      std::vector<lang::mir_dominance_detail::CfgNode *>::const_iterator;

  static NodeRef
  getEntryNode(const lang::mir_dominance_detail::CfgSnapshot *snapshot) {
    return &snapshot->front();
  }

  static unsigned
  getMaxNumber(const lang::mir_dominance_detail::CfgSnapshot *snapshot) {
    return static_cast<unsigned>(snapshot->nodes.size());
  }

  static unsigned
  getNumberEpoch(const lang::mir_dominance_detail::CfgSnapshot *) {
    return 0;
  }

  static nodes_iterator
  nodes_begin(const lang::mir_dominance_detail::CfgSnapshot *snapshot) {
    return snapshot->nodePointers.begin();
  }

  static nodes_iterator
  nodes_end(const lang::mir_dominance_detail::CfgSnapshot *snapshot) {
    return snapshot->nodePointers.end();
  }
};

} // namespace llvm

namespace lang {

bool MirDominanceInfo::dominates(MirBlockId dominator, MirBlockId block) const {
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

namespace {

using CfgNode = mir_dominance_detail::CfgNode;
using CfgSnapshot = mir_dominance_detail::CfgSnapshot;

[[nodiscard]] bool validBlockId(const MirBody &body, MirBlockId id) {
  return id != 0 && id <= body.blocks.size();
}

[[nodiscard]] std::optional<std::vector<MirBlockId>>
successorsForDominance(const MirBody &body, const MirTerminator &terminator) {
  std::vector<MirBlockId> result;
  switch (terminator.kind) {
  case MirTerminatorKind::Goto:
    result.push_back(terminator.target);
    break;
  case MirTerminatorKind::Branch:
    result.push_back(terminator.target);
    result.push_back(terminator.elseTarget);
    break;
  case MirTerminatorKind::Switch:
    result.push_back(terminator.target);
    for (const MirSwitchTarget &target : terminator.switchTargets) {
      result.push_back(target.target);
    }
    break;
  case MirTerminatorKind::Invoke:
    result.push_back(terminator.target);
    result.push_back(terminator.elseTarget);
    break;
  case MirTerminatorKind::Return:
  case MirTerminatorKind::PropagateFailure:
  case MirTerminatorKind::Unreachable:
  case MirTerminatorKind::Exit:
  case MirTerminatorKind::ContainFailure:
  case MirTerminatorKind::TerminateCleanupFailure:
    break;
  case MirTerminatorKind::None:
    return std::nullopt;
  }

  if (std::any_of(result.begin(), result.end(), [&](MirBlockId target) {
        return !validBlockId(body, target);
      })) {
    return std::nullopt;
  }
  std::vector<MirBlockId> unique;
  unique.reserve(result.size());
  for (const MirBlockId target : result) {
    if (std::find(unique.begin(), unique.end(), target) == unique.end()) {
      unique.push_back(target);
    }
  }
  return unique;
}

[[nodiscard]] std::optional<CfgSnapshot> makeSnapshot(const MirBody &body) {
  if (!validBlockId(body, body.entry) ||
      body.blocks.size() > std::numeric_limits<unsigned>::max()) {
    return std::nullopt;
  }

  CfgSnapshot snapshot;
  snapshot.entry = body.entry;
  snapshot.nodes.resize(body.blocks.size());
  snapshot.nodePointers.reserve(body.blocks.size());
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    if (body.blocks[index].id != index + 1) {
      return std::nullopt;
    }
    snapshot.nodes[index].id = index + 1;
    snapshot.nodes[index].parent = &snapshot;
    snapshot.nodePointers.push_back(&snapshot.nodes[index]);
  }

  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    const std::optional<std::vector<MirBlockId>> successors =
        successorsForDominance(body, body.blocks[index].terminator);
    if (!successors) {
      return std::nullopt;
    }
    CfgNode &from = snapshot.nodes[index];
    from.successors.reserve(successors->size());
    for (const MirBlockId target : *successors) {
      CfgNode &to = snapshot.nodes[target - 1];
      from.successors.push_back(&to);
      to.predecessors.push_back(&from);
    }
  }
  return snapshot;
}

} // namespace

std::optional<MirDominanceInfo> computeMirDominance(const MirBody &body) {
  std::optional<CfgSnapshot> snapshot = makeSnapshot(body);
  if (!snapshot) {
    return std::nullopt;
  }

  // Moving a snapshot would invalidate every private node's parent pointer.
  // Rebind it once after optional construction; edges remain valid because the
  // nodes vector transfers its allocation when the snapshot moves.
  for (CfgNode &node : snapshot->nodes) {
    node.parent = &*snapshot;
  }

  llvm::DominatorTreeBase<CfgNode, false> tree;
  tree.recalculate(*snapshot);

  MirDominanceInfo result;
  result.immediateDominators.assign(body.blocks.size(), 0);
  result.reachableBlocks.assign(body.blocks.size(), false);
  for (CfgNode &node : snapshot->nodes) {
    const llvm::DomTreeNodeBase<CfgNode> *treeNode = tree.getNode(&node);
    if (treeNode == nullptr) {
      continue;
    }
    result.reachableBlocks[node.id - 1] = true;
    const llvm::DomTreeNodeBase<CfgNode> *immediate = treeNode->getIDom();
    if (immediate != nullptr && immediate->getBlock() != nullptr) {
      result.immediateDominators[node.id - 1] = immediate->getBlock()->id;
    }
  }
  return result;
}

} // namespace lang
