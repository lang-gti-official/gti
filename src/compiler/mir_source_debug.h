#pragma once

#include "gti/mir.h"

#include <span>
#include <unordered_map>

namespace lang::mir_detail {

class MirSourceAnchorIndex {
public:
  explicit MirSourceAnchorIndex(const MirBody &body)
      : MirSourceAnchorIndex(&body) {}
  explicit MirSourceAnchorIndex(const MirBody *body) {
    if (!body || !body->sourceDebug.tables)
      return;
    for (const auto &anchor : body->sourceDebug.tables->statements)
      statements.emplace(anchor.id, &anchor);
    for (const auto &anchor : body->sourceDebug.tables->values)
      values.emplace(anchor.id, &anchor);
  }

  [[nodiscard]] const SourceDebugAnchor *find(HirStatementId statement,
                                              HirValueId value,
                                              bool writtenOnly = false) const {
    const auto accepted = [&](const auto &found, const auto &rows) {
      return found != rows.end() && (!writtenOnly || written(*found->second));
    };
    if (const auto found = statements.find(statement);
        accepted(found, statements))
      return found->second;
    if (const auto found = values.find(value); accepted(found, values))
      return found->second;
    return nullptr;
  }

  [[nodiscard]] static bool written(const SourceDebugAnchor &anchor) {
    const auto &span = anchor.source;
    return !span.source.empty() && span.source.front() != '<' &&
           span.start < span.end && span.line > 0;
  }

private:
  std::unordered_map<HirStatementId, const SourceDebugAnchor *> statements;
  std::unordered_map<HirValueId, const SourceDebugAnchor *> values;
};

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

// Original-MIR events use actual reads/writes of unaliased scalar bindings.
[[nodiscard]] std::vector<MirSourceValueEvent>
deriveMirSourceValueEvents(const MirBody &body);

struct MirDebugRepairUndo {
  struct Local {
    std::size_t index = 0;
    MirPlaceId place = 0;
    MirSourceLocalPolicy policy = MirSourceLocalPolicy::Unproven;
  };
  std::vector<Local> locals;
  std::vector<std::pair<std::size_t, MirSourceLocalEnd>> ends;
  std::vector<std::pair<std::size_t, MirSourceValueEvent>> valueEvents;
  void restore(MirBody &body) const;
};

// No allocation request: losing an exact anchor invalidates affected value
// locations. The in-place dead mutator retains only this bounded
// journal, not a copy of every declaration and scope for each instruction.
[[nodiscard]] MirDebugRepairUndo repairMirSourceDebug(
    const MirBody &before, MirBody &candidate, const MirDebugEditEffect &effect);

} // namespace lang::mir_detail
