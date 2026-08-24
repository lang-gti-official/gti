#pragma once

#include "gti/mir.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace lang {

// Stable identity for one executable transfer edge. `ordinal` is local to the
// source block and lets clients distinguish repeated targets (for example,
// switch arms) without relying on pointer identity.
struct MirDataflowEdgeId {
  MirBlockId source = 0;
  MirBlockId target = 0;
  std::size_t ordinal = 0;

  friend bool operator==(const MirDataflowEdgeId &,
                         const MirDataflowEdgeId &) = default;
  friend auto operator<=>(const MirDataflowEdgeId &,
                          const MirDataflowEdgeId &) = default;
};

template <typename State> struct MirForwardDataflowEdge {
  MirBlockId target = 0;
  std::size_t ordinal = 0;
  State state;
};

struct MirDataflowMergeContext {
  MirDataflowEdgeId edge;
  std::size_t targetUpdateCount = 0;
  bool widen = false;
};

struct MirForwardDataflowOptions {
  // A zero iteration limit selects max(128, block-count * 64).
  std::size_t iterationLimit = 0;
  // Widening starts when this many prior updates reached the target block.
  // A zero threshold disables widening.
  std::size_t widenAfterUpdates = 4;
};

enum class MirDataflowStatus {
  Converged,
  InvalidControlFlow,
  NonMonotoneTransfer,
  IterationLimit,
};

template <typename State> struct MirForwardDataflowResult {
  MirDataflowStatus status = MirDataflowStatus::InvalidControlFlow;
  std::vector<std::optional<State>> incoming;
  std::vector<MirDataflowEdgeId> executableEdges;
  std::vector<std::size_t> updateCounts;
  std::size_t iterations = 0;

  [[nodiscard]] bool converged() const {
    return status == MirDataflowStatus::Converged;
  }

  [[nodiscard]] bool executable(MirDataflowEdgeId edge) const {
    return std::binary_search(executableEdges.begin(), executableEdges.end(),
                              edge);
  }
};

namespace mir_dataflow_detail {

[[nodiscard]] inline bool validEdge(const MirBody &body, MirBlockId target,
                                    const std::vector<MirOperand> &arguments) {
  return target != 0 && target <= body.blocks.size() &&
         body.blocks[target - 1].parameters.size() == arguments.size();
}

[[nodiscard]] inline bool validControlFlow(const MirBody &body,
                                           const MirBlock &block) {
  const MirTerminator &terminator = block.terminator;
  switch (terminator.kind) {
  case MirTerminatorKind::Goto:
    return validEdge(body, terminator.target, terminator.targetArguments);
  case MirTerminatorKind::Branch:
  case MirTerminatorKind::Invoke:
    return validEdge(body, terminator.target, terminator.targetArguments) &&
           validEdge(body, terminator.elseTarget, terminator.elseArguments);
  case MirTerminatorKind::Switch:
    return validEdge(body, terminator.target, terminator.targetArguments) &&
           std::all_of(
               terminator.switchTargets.begin(), terminator.switchTargets.end(),
               [&](const MirSwitchTarget &target) {
                 return validEdge(body, target.target, target.arguments);
               });
  case MirTerminatorKind::Return:
  case MirTerminatorKind::PropagateFailure:
  case MirTerminatorKind::Unreachable:
  case MirTerminatorKind::Exit:
  case MirTerminatorKind::ContainFailure:
  case MirTerminatorKind::TerminateCleanupFailure:
    return true;
  case MirTerminatorKind::None:
    return false;
  }
  return false;
}

[[nodiscard]] inline bool
canonicalEdge(const MirBlock &block, MirBlockId target, std::size_t ordinal) {
  const MirTerminator &terminator = block.terminator;
  switch (terminator.kind) {
  case MirTerminatorKind::Goto:
    return ordinal == 0 && target == terminator.target;
  case MirTerminatorKind::Branch:
  case MirTerminatorKind::Invoke:
    return (ordinal == 0 && target == terminator.target) ||
           (ordinal == 1 && target == terminator.elseTarget);
  case MirTerminatorKind::Switch:
    if (ordinal == 0) {
      return target == terminator.target;
    }
    return ordinal <= terminator.switchTargets.size() &&
           target == terminator.switchTargets[ordinal - 1].target;
  case MirTerminatorKind::None:
  case MirTerminatorKind::Return:
  case MirTerminatorKind::PropagateFailure:
  case MirTerminatorKind::Unreachable:
  case MirTerminatorKind::Exit:
  case MirTerminatorKind::ContainFailure:
  case MirTerminatorKind::TerminateCleanupFailure:
    return false;
  }
  return false;
}

} // namespace mir_dataflow_detail

// Deterministic monotone forward solver for body-local MIR analyses.
//
// `transfer(block, incoming)` returns only currently executable outgoing
// edges. Once a canonical edge slot is executable for a source, later
// transfers for that source must retain it. `merge(target, incoming, context)`
// must implement a monotone join and return true exactly when `target` changed.
// Finite lattices may ignore context.widen; interval-like clients use it to
// guarantee bounded ascent.
// The solver owns scheduling, executable-edge tracking, widening cadence, and
// convergence limits, while clients retain authority over their typed domain.
template <typename State, typename Transfer, typename Merge>
[[nodiscard]] MirForwardDataflowResult<State>
runMirForwardDataflow(const MirBody &body, State entryState,
                      Transfer &&transfer, Merge &&merge,
                      MirForwardDataflowOptions options = {}) {
  MirForwardDataflowResult<State> result;
  result.incoming.resize(body.blocks.size());
  result.updateCounts.resize(body.blocks.size());
  if (body.entry == 0 || body.entry > body.blocks.size()) {
    return result;
  }
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    if (body.blocks[index].id != index + 1 ||
        !mir_dataflow_detail::validControlFlow(body, body.blocks[index])) {
      return result;
    }
  }

  result.incoming[body.entry - 1] = std::move(entryState);
  std::deque<MirBlockId> worklist{body.entry};
  std::vector<bool> queued(body.blocks.size(), false);
  std::vector<std::vector<MirDataflowEdgeId>> sourceExecutable(
      body.blocks.size());
  queued[body.entry - 1] = true;
  const std::size_t iterationLimit =
      options.iterationLimit == 0
          ? std::max<std::size_t>(128, body.blocks.size() * 64)
          : options.iterationLimit;

  while (!worklist.empty()) {
    if (result.iterations >= iterationLimit) {
      result.status = MirDataflowStatus::IterationLimit;
      return result;
    }
    ++result.iterations;
    const MirBlockId source = worklist.front();
    worklist.pop_front();
    queued[source - 1] = false;
    const MirBlock *block = body.findBlock(source);
    if (block == nullptr || !result.incoming[source - 1]) {
      result.status = MirDataflowStatus::InvalidControlFlow;
      return result;
    }

    std::vector<MirForwardDataflowEdge<State>> edges =
        transfer(*block, *result.incoming[source - 1]);
    std::stable_sort(edges.begin(), edges.end(),
                     [](const MirForwardDataflowEdge<State> &left,
                        const MirForwardDataflowEdge<State> &right) {
                       return std::pair{left.ordinal, left.target} <
                              std::pair{right.ordinal, right.target};
                     });
    std::vector<MirDataflowEdgeId> currentExecutable;
    currentExecutable.reserve(edges.size());
    for (const MirForwardDataflowEdge<State> &edge : edges) {
      if (edge.target == 0 || edge.target > body.blocks.size() ||
          !mir_dataflow_detail::canonicalEdge(*block, edge.target,
                                              edge.ordinal)) {
        result.status = MirDataflowStatus::InvalidControlFlow;
        return result;
      }
      currentExecutable.push_back(MirDataflowEdgeId{
          .source = source, .target = edge.target, .ordinal = edge.ordinal});
    }
    std::sort(currentExecutable.begin(), currentExecutable.end());
    if (std::adjacent_find(currentExecutable.begin(),
                           currentExecutable.end()) !=
        currentExecutable.end()) {
      result.status = MirDataflowStatus::InvalidControlFlow;
      return result;
    }
    const std::vector<MirDataflowEdgeId> &previousExecutable =
        sourceExecutable[source - 1];
    if (!std::includes(currentExecutable.begin(), currentExecutable.end(),
                       previousExecutable.begin(), previousExecutable.end())) {
      result.status = MirDataflowStatus::NonMonotoneTransfer;
      return result;
    }
    sourceExecutable[source - 1] = std::move(currentExecutable);

    for (MirForwardDataflowEdge<State> &edge : edges) {
      const MirDataflowEdgeId identity{
          .source = source, .target = edge.target, .ordinal = edge.ordinal};
      const auto insertion =
          std::lower_bound(result.executableEdges.begin(),
                           result.executableEdges.end(), identity);
      if (insertion == result.executableEdges.end() || *insertion != identity) {
        result.executableEdges.insert(insertion, identity);
      }

      std::optional<State> &target = result.incoming[edge.target - 1];
      bool changed = false;
      if (!target) {
        target = std::move(edge.state);
        changed = true;
      } else {
        const std::size_t updates = result.updateCounts[edge.target - 1];
        changed = merge(*target, edge.state,
                        {.edge = identity,
                         .targetUpdateCount = updates,
                         .widen = options.widenAfterUpdates != 0 &&
                                  updates >= options.widenAfterUpdates});
      }
      if (!changed) {
        continue;
      }
      ++result.updateCounts[edge.target - 1];
      if (!queued[edge.target - 1]) {
        worklist.push_back(edge.target);
        queued[edge.target - 1] = true;
      }
    }
  }

  result.status = MirDataflowStatus::Converged;
  return result;
}

} // namespace lang
