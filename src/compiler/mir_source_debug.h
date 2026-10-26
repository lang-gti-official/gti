#pragma once

#include "gti/mir.h"

#include <span>

namespace lang::mir_detail {

// Deterministic, pointer-free description of already verified HIR source facts.
// No execution availability is copied or inferred here.
[[nodiscard]] SourceDebugRecord<MirSourceDebugTables>
describeHirSourceDebug(const HirBody &body);

[[nodiscard]] bool sourceDebugMatchesHir(const HirBody &source,
                                       const MirBody &body);

struct MirDebugEditEffect {
  MirPlaceId promotedPlace = 0;
  std::span<const MirInstructionId> removedInstructions;
  std::span<const MirBlockId> removedBlocks;
};

struct MirDebugRepairUndo {
  struct Local {
    std::size_t index = 0;
    MirPlaceId place = 0;
    MirSourceLocalPolicy policy = MirSourceLocalPolicy::Unproven;
  };
  std::vector<Local> locals;
  std::vector<std::pair<std::size_t, MirSourceLocalEnd>> ends;
  void restore(MirBody &body) const;
};

// No SSA recovery or allocation request: losing an exact anchor invalidates
// affected value locations. The in-place dead mutator retains only this bounded
// journal, not a copy of every declaration and scope for each instruction.
[[nodiscard]] MirDebugRepairUndo repairMirSourceDebug(
    const MirBody &before, MirBody &candidate, const MirDebugEditEffect &effect);

} // namespace lang::mir_detail
