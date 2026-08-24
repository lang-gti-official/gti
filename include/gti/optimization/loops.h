#pragma once

#include "gti/mir.h"
#include "gti/mir_dominance.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lang {

struct MirRelationalRangeFact {
  MirValueId value = 0;
  MirOperation relation = MirOperation::None;
  MirOperand bound;
  MirBlockId conditionBlock = 0;
  MirBlockId constrainedSuccessor = 0;

  friend bool operator==(const MirRelationalRangeFact &,
                         const MirRelationalRangeFact &) = default;
};

struct MirAffineInductionFact {
  MirValueId parameter = 0;
  MirBlockId header = 0;
  MirBlockId latch = 0;
  MirOperand initial;
  std::uint64_t stepMagnitude = 0;
  bool decreasing = false;
  std::optional<MirRelationalRangeFact> range;
  std::vector<MirInstructionId> relatedInstructions;
  std::vector<MirValueId> relatedValues;

  friend bool operator==(const MirAffineInductionFact &,
                         const MirAffineInductionFact &) = default;
};

struct MirNaturalLoop {
  MirBlockId header = 0;
  std::vector<MirBlockId> latches;
  std::vector<MirBlockId> blocks;
  std::optional<std::size_t> parent;
  std::vector<std::size_t> children;
  std::size_t depth = 0;
  std::vector<MirValueId> invariantValues;
  std::vector<MirAffineInductionFact> inductions;

  [[nodiscard]] bool contains(MirBlockId block) const;

  friend bool operator==(const MirNaturalLoop &,
                         const MirNaturalLoop &) = default;
};

struct MirLoopAnalysis {
  std::vector<MirNaturalLoop> loops;
  bool hasIrreducibleControlFlow = false;

  [[nodiscard]] const MirNaturalLoop *loopForHeader(MirBlockId header) const;

  friend bool operator==(const MirLoopAnalysis &,
                         const MirLoopAnalysis &) = default;
};

[[nodiscard]] MirLoopAnalysis
analyzeMirLoops(const MirBody &body, const MirDominanceInfo &dominance);

} // namespace lang
