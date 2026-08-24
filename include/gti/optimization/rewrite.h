#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace lang {

struct MirInstructionKey {
  MirBodyAddress body;
  MirInstructionId instruction = 0;

  friend bool operator==(const MirInstructionKey &,
                         const MirInstructionKey &) = default;
};

struct MirAnalysisInvalidation {
  bool instructionFacts = false;
  bool valueUses = false;
  bool controlFlow = false;
  bool reachability = false;
  bool dominance = false;
};

struct MirEditResult {
  bool changed = false;
  std::size_t appliedPatches = 0;
  bool valueUsesRebuilt = false;
  MirAnalysisInvalidation invalidation;
  std::vector<MirTransformRecord> transformations;
  MirVerificationResult verification;

  [[nodiscard]] bool valid() const { return verification.valid(); }
};

// The controlled MIR editor exposes only edits with implemented transformation
// clients and independent coherence replay. It accumulates stable-identity
// claims against an immutable snapshot and commits them atomically after repair
// and verification. General insertion, arbitrary erasure/CFG editing, and
// mutable body access remain outside this interface.
class MirProgramEditor final {
public:
  MirProgramEditor(MirProgram &program, std::string pass)
      : program(program), pass(std::move(pass)) {}

  // Compatibility accessors delegate to core MIR navigation. New passes can
  // use enumerateMirBodyAddresses/findMirBody without constructing an editor.
  [[nodiscard]] std::vector<MirBodyAddress> bodies() const {
    return enumerateMirBodyAddresses(program);
  }
  [[nodiscard]] const MirBody *body(MirBodyAddress address) const {
    return findMirBody(program, address);
  }

  void queueLiteralReplacement(MirInstructionKey target,
                               MirOperation expectedOperation, Literal literal);

  // Queues the replacement of one exact compute instruction with its folded
  // literal; the fold's operation and value operands are retained as the
  // ComputeFold provenance the verifier replays.
  void queueComputeFoldReplacement(MirInstructionKey target,
                                   MirOperation expectedOperation,
                                   Literal literal);

  // Queues the rewrite of one Branch terminator whose condition value's
  // definition is a dominating literal bool into a Goto to the taken
  // target, retaining the condition as BranchFold provenance. Block
  // reachability recomputes atomically with the application.
  void queueBranchFold(MirBodyAddress body, MirBlockId block,
                       MirValueId expectedCondition, bool taken);

  // Queues one canonical scalar-to-SSA rewrite. The editor and coherence
  // verifier independently rerun the core eligibility/rewrite authority from
  // the pre-transform body; the pass-provided place ID is only a claim.
  void queueScalarPromotion(MirBodyAddress body, MirPlaceId place);

  // Queues canonical destructive cleanup. Both operations preserve stable MIR
  // identities through tombstones and are accepted only when the current
  // value-use/effect or reachability facts prove the removal harmless.
  void queueDeadInstructionRemoval(MirInstructionKey target);
  void queueUnreachableBlockRemoval(MirBodyAddress body, MirBlockId block);

  [[nodiscard]] std::size_t pendingPatchCount() const { return patches.size(); }
  [[nodiscard]] std::size_t pendingBranchFoldCount() const {
    return branchFolds.size();
  }
  [[nodiscard]] std::size_t pendingScalarPromotionCount() const {
    return scalarPromotions.size();
  }
  [[nodiscard]] std::size_t pendingDeadInstructionCount() const {
    return deadInstructions.size();
  }
  [[nodiscard]] std::size_t pendingUnreachableBlockCount() const {
    return unreachableBlocks.size();
  }

  [[nodiscard]] MirEditResult apply();

private:
  struct LiteralReplacement {
    MirInstructionKey target;
    MirOperation expectedOperation = MirOperation::None;
    Literal literal;
    bool computeFold = false;
  };

  struct BranchFold {
    MirBodyAddress body;
    MirBlockId block = 0;
    MirValueId expectedCondition = 0;
    bool taken = false;
  };

  struct ScalarPromotion {
    MirBodyAddress body;
    MirPlaceId place = 0;
  };

  struct UnreachableBlock {
    MirBodyAddress body;
    MirBlockId block = 0;
  };

  MirProgram &program;
  std::string pass;
  std::vector<LiteralReplacement> patches;
  std::vector<BranchFold> branchFolds;
  std::vector<ScalarPromotion> scalarPromotions;
  std::vector<MirInstructionKey> deadInstructions;
  std::vector<UnreachableBlock> unreachableBlocks;
};

} // namespace lang
