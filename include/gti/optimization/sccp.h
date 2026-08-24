#pragma once

#include "gti/mir.h"
#include "gti/optimization/dataflow.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace lang {

enum class MirSccpLatticeKind : std::uint8_t {
  Unknown,
  Constant,
  Overdefined,
};

struct MirSccpEvidence {
  std::vector<MirBlockId> blocks;
  std::vector<MirInstructionId> instructions;
  std::vector<MirValueId> values;

  friend bool operator==(const MirSccpEvidence &,
                         const MirSccpEvidence &) = default;
};

struct MirSccpValueFact {
  MirValueId value = 0;
  SemanticType type = SemanticType::Unknown;
  MirSccpLatticeKind kind = MirSccpLatticeKind::Unknown;
  std::optional<Literal> literal;
  MirSccpEvidence evidence;

  friend bool operator==(const MirSccpValueFact &,
                         const MirSccpValueFact &) = default;
};

struct MirSccpBranchFact {
  MirBlockId block = 0;
  MirValueId condition = 0;
  bool taken = false;
  MirSccpEvidence evidence;

  friend bool operator==(const MirSccpBranchFact &,
                         const MirSccpBranchFact &) = default;
};

struct MirSccpAnalysis {
  bool complete = false;
  std::vector<MirSccpValueFact> values;
  std::vector<MirSccpBranchFact> branches;
  std::vector<MirBlockId> executableBlocks;
  std::vector<MirDataflowEdgeId> executableEdges;

  [[nodiscard]] const MirSccpValueFact *find(MirValueId value) const;
  [[nodiscard]] const MirSccpBranchFact *branch(MirBlockId block) const;

  friend bool operator==(const MirSccpAnalysis &,
                         const MirSccpAnalysis &) = default;
};

// Sparse conditional constant propagation over scalar, non-owning MIR SSA.
// Unknown is the lattice bottom, Overdefined is top, and exact literals meet
// only when their type and value agree. Unsupported operations become top.
[[nodiscard]] MirSccpAnalysis analyzeMirSccp(const MirProgram &program,
                                             MirBodyAddress address);

} // namespace lang
