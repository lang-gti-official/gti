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

// Deterministic monotone forward solver for body-local MIR analyses.
//
// `transfer(block, incoming)` returns only currently executable outgoing
// edges. `merge(target, incoming, context)` must implement a monotone join and
// return true exactly when `target` changed. Finite lattices may ignore
// context.widen; interval-like clients use it to guarantee bounded ascent.
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
    if (body.blocks[index].id != index + 1) {
      return result;
    }
  }

  result.incoming[body.entry - 1] = std::move(entryState);
  std::deque<MirBlockId> worklist{body.entry};
  std::vector<bool> queued(body.blocks.size(), false);
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
    std::optional<MirDataflowEdgeId> previousEdge;
    for (MirForwardDataflowEdge<State> &edge : edges) {
      if (edge.target == 0 || edge.target > body.blocks.size()) {
        result.status = MirDataflowStatus::InvalidControlFlow;
        return result;
      }
      const MirDataflowEdgeId identity{
          .source = source, .target = edge.target, .ordinal = edge.ordinal};
      if (previousEdge && *previousEdge == identity) {
        result.status = MirDataflowStatus::InvalidControlFlow;
        return result;
      }
      previousEdge = identity;
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
