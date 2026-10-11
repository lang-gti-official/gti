#include "cpp_mir_control_flow.h"

#include <algorithm>
#include <utility>

namespace lang {
namespace {

void appendEdges(const MirTerminator &terminator,
                 std::vector<CppMirControlFlowEdge> &edges) {
  const auto append = [&](MirBlockId target, std::size_t ordinal) {
    edges.push_back({.target = target, .ordinal = ordinal});
  };
  switch (terminator.kind) {
  case MirTerminatorKind::Goto:
  case MirTerminatorKind::ContinueFailure:
    append(terminator.target, 0);
    return;
  case MirTerminatorKind::Branch:
  case MirTerminatorKind::Invoke:
    append(terminator.target, 0);
    append(terminator.elseTarget, 1);
    return;
  case MirTerminatorKind::Switch:
    append(terminator.target, 0);
    for (std::size_t index = 0; index < terminator.switchTargets.size();
         ++index) {
      append(terminator.switchTargets[index].target, index + 1);
    }
    return;
  case MirTerminatorKind::Return:
  case MirTerminatorKind::PropagateFailure:
  case MirTerminatorKind::Unreachable:
  case MirTerminatorKind::Exit:
  case MirTerminatorKind::ContainFailure:
  case MirTerminatorKind::TerminateCleanupFailure:
  case MirTerminatorKind::None:
    return;
  }
}

void addIssue(CppMirControlFlowVerification &result,
              CppMirControlFlowIssueKind kind, std::string detail,
              MirBlockId block = 0, std::size_t edgeOrdinal = 0) {
  result.issues.push_back({.kind = kind,
                           .block = block,
                           .edgeOrdinal = edgeOrdinal,
                           .detail = std::move(detail)});
}

[[nodiscard]] bool validTarget(const MirBody &body, MirBlockId target) {
  return target != 0 && target <= body.blocks.size() &&
         body.blocks[target - 1].id == target;
}

void validateMirControlFlow(const MirBody &body,
                            CppMirControlFlowVerification &result) {
  if (body.blocks.empty() || !validTarget(body, body.entry)) {
    addIssue(result, CppMirControlFlowIssueKind::InvalidMirBody,
             "MIR body has no canonical entry block");
  }
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    const MirBlock &block = body.blocks[index];
    if (block.id != index + 1) {
      addIssue(result, CppMirControlFlowIssueKind::InvalidMirBody,
               "MIR block inventory is not canonical", block.id);
    }
    if (block.terminator.kind == MirTerminatorKind::None) {
      addIssue(result, CppMirControlFlowIssueKind::InvalidMirBody,
               "MIR block has no executable terminator", block.id);
      continue;
    }
    std::vector<CppMirControlFlowEdge> edges;
    appendEdges(block.terminator, edges);
    for (const CppMirControlFlowEdge &edge : edges) {
      if (!validTarget(body, edge.target)) {
        addIssue(result, CppMirControlFlowIssueKind::InvalidMirBody,
                 "MIR successor does not name a canonical block", block.id,
                 edge.ordinal);
      }
    }
  }
}

} // namespace

std::string cppMirControlFlowLabel(MirBlockId block) {
  return "__gti_mir_bb_" + std::to_string(block);
}

CppMirControlFlowPlan buildCppMirControlFlowPlan(const MirBody &body) {
  CppMirControlFlowPlan result{.entry = body.entry};
  result.blocks.reserve(body.blocks.size());
  for (const MirBlock &block : body.blocks) {
    CppMirControlFlowBlock row{.block = block.id,
                               .label = cppMirControlFlowLabel(block.id)};
    appendEdges(block.terminator, row.outgoing);
    result.blocks.push_back(std::move(row));
  }
  return result;
}

CppMirControlFlowVerification
verifyCppMirControlFlowPlan(const MirBody &body,
                            const CppMirControlFlowPlan &plan) {
  CppMirControlFlowVerification result;
  validateMirControlFlow(body, result);

  const CppMirControlFlowPlan expected = buildCppMirControlFlowPlan(body);
  if (plan.entry != expected.entry) {
    addIssue(result, CppMirControlFlowIssueKind::EntryCoverage,
             "control-flow plan entry does not match MIR entry", plan.entry);
  }
  if (plan.blocks.size() != expected.blocks.size()) {
    addIssue(result, CppMirControlFlowIssueKind::BlockCoverage,
             "control-flow plan does not cover every MIR block exactly once");
  }

  const std::size_t common =
      std::min(plan.blocks.size(), expected.blocks.size());
  for (std::size_t index = 0; index < common; ++index) {
    const CppMirControlFlowBlock &actual = plan.blocks[index];
    const CppMirControlFlowBlock &wanted = expected.blocks[index];
    if (actual.block != wanted.block) {
      addIssue(result, CppMirControlFlowIssueKind::BlockCoverage,
               "control-flow plan block order or identity differs from MIR",
               actual.block);
      continue;
    }
    if (actual.label != wanted.label) {
      addIssue(result, CppMirControlFlowIssueKind::LabelCoverage,
               "control-flow plan block label is not canonical", actual.block);
    }
    if (actual.outgoing.size() != wanted.outgoing.size()) {
      addIssue(result, CppMirControlFlowIssueKind::EdgeCoverage,
               "control-flow plan does not cover every successor edge exactly "
               "once",
               actual.block);
    }
    const std::size_t edgeCommon =
        std::min(actual.outgoing.size(), wanted.outgoing.size());
    for (std::size_t edgeIndex = 0; edgeIndex < edgeCommon; ++edgeIndex) {
      if (actual.outgoing[edgeIndex] != wanted.outgoing[edgeIndex]) {
        addIssue(result, CppMirControlFlowIssueKind::EdgeCoverage,
                 "control-flow plan successor target or canonical slot differs "
                 "from MIR",
                 actual.block, actual.outgoing[edgeIndex].ordinal);
      }
    }
  }
  return result;
}

const CppMirControlFlowBlock *
findCppMirControlFlowBlock(const CppMirControlFlowPlan &plan,
                           MirBlockId block) {
  // A plan lists MIR's blocks in order, and MIR block ids are positions.
  if (block != 0 && block <= plan.blocks.size() &&
      plan.blocks[block - 1].block == block) {
    return &plan.blocks[block - 1];
  }
  const auto found =
      std::find_if(plan.blocks.begin(), plan.blocks.end(),
                   [block](const auto &row) { return row.block == block; });
  return found == plan.blocks.end() ? nullptr : &*found;
}

const CppMirControlFlowEdge *
findCppMirControlFlowEdge(const CppMirControlFlowPlan &plan, MirBlockId source,
                          std::size_t ordinal) {
  const CppMirControlFlowBlock *block =
      findCppMirControlFlowBlock(plan, source);
  if (block == nullptr) {
    return nullptr;
  }
  const auto found = std::find_if(
      block->outgoing.begin(), block->outgoing.end(),
      [ordinal](const auto &edge) { return edge.ordinal == ordinal; });
  return found == block->outgoing.end() ? nullptr : &*found;
}

} // namespace lang
