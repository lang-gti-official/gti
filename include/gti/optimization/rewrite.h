#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <vector>

namespace lang {

struct MirInstructionAddress {
  MirBodyAddress body;
  MirBlockId block = 0;
  std::size_t index = 0;

  friend bool operator==(const MirInstructionAddress &,
                         const MirInstructionAddress &) = default;
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
  MirVerificationResult verification;

  [[nodiscard]] bool valid() const { return verification.valid(); }
};

// The first controlled MIR editor deliberately exposes only the replacement
// operation needed by literal identity folding. It accumulates guarded patches
// against an immutable snapshot and commits them atomically after repair and
// verification. Insertion, erasure, CFG editing, and mutable body access remain
// outside this interface until a concrete transform requires them.
class MirProgramEditor final {
public:
  explicit MirProgramEditor(MirProgram &program) : program(program) {}

  // Compatibility accessors delegate to core MIR navigation. New passes can
  // use enumerateMirBodyAddresses/findMirBody without constructing an editor.
  [[nodiscard]] std::vector<MirBodyAddress> bodies() const {
    return enumerateMirBodyAddresses(program);
  }
  [[nodiscard]] const MirBody *body(MirBodyAddress address) const {
    return findMirBody(program, address);
  }

  void queueLiteralReplacement(MirInstructionAddress address,
                               MirInstructionId expectedInstruction,
                               MirOperation expectedOperation, Literal literal);

  // Queues the replacement of one exact compute instruction with its folded
  // literal; the fold's operation and value operands are retained as the
  // ComputeFold provenance the verifier replays.
  void queueComputeFoldReplacement(MirInstructionAddress address,
                                   MirInstructionId expectedInstruction,
                                   MirOperation expectedOperation,
                                   Literal literal);

  // Queues the rewrite of one Branch terminator whose condition value's
  // definition is a dominating literal bool into a Goto to the taken
  // target, retaining the condition as BranchFold provenance. Block
  // reachability recomputes atomically with the application.
  void queueBranchFold(MirBodyAddress body, MirBlockId block,
                       MirValueId expectedCondition, bool taken);

  [[nodiscard]] std::size_t pendingPatchCount() const { return patches.size(); }

  [[nodiscard]] MirEditResult apply();

private:
  struct LiteralReplacement {
    MirInstructionAddress address;
    MirInstructionId expectedInstruction = 0;
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

  MirProgram &program;
  std::vector<LiteralReplacement> patches;
  std::vector<BranchFold> branchFolds;
};

} // namespace lang
