// Dominator subtree intervals agree with walking immediate dominators,
// regardless of sibling order.
#include "gti/mir_dominance.h"
#include "unit_test.h"

#include <iostream>
#include <utility>

GTI_UNIT_TEST(mir_dominance_intervals) {
  using namespace lang;
  // A diamond joins at 6, which loops back to 4 or exits through 1.
  // Block 3 is an unreachable predecessor of 1. The entry is block 2.
  MirBody body;
  body.entry = 2;
  body.blocks = {
      {.id = 1, .terminator = {.kind = MirTerminatorKind::Return}},
      {.id = 2,
       .terminator = {.kind = MirTerminatorKind::Branch,
                      .target = 4,
                      .elseTarget = 5}},
      {.id = 3, .terminator = {.kind = MirTerminatorKind::Goto, .target = 1}},
      {.id = 4, .terminator = {.kind = MirTerminatorKind::Goto, .target = 6}},
      {.id = 5, .terminator = {.kind = MirTerminatorKind::Goto, .target = 6}},
      {.id = 6,
       .terminator = {
           .kind = MirTerminatorKind::Branch, .target = 4, .elseTarget = 1}}};
  auto dominance = computeMirDominance(body);
  if (!dominance || dominance->isReachable(3) ||
      dominance->immediateDominator(1) != 6)
    return 1;
  for (MirBlockId block = 0; block <= body.blocks.size() + 1; ++block) {
    for (MirBlockId ancestor = 0; ancestor <= body.blocks.size() + 1;
         ++ancestor) {
      bool expected = false;
      if (dominance->isReachable(block)) {
        for (auto current = block; current != 0;
             current = dominance->immediateDominator(current)) {
          if (current == ancestor) {
            expected = true;
            break;
          }
        }
      }
      if (dominance->dominates(ancestor, block) != expected) {
        std::cerr << "incorrect dominance for " << ancestor << " over " << block
                  << '\n';
        return 1;
      }
    }
  }
  auto &branch = body.blocks[1].terminator;
  std::swap(branch.target, branch.elseTarget);
  if (computeMirDominance(body) != dominance)
    return 1;
  return 0;
}
