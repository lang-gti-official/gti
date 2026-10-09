#include "../src/compiler/optimization/evidence_set.h"
#include "gti/frontend.h"
#include "gti/mir_dominance.h"
#include "gti/optimization/alias.h"
#include "gti/optimization/dataflow.h"
#include "gti/optimization/loops.h"
#include "gti/optimization/safety.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

struct DataflowTestState {
  int upper = 0;

  friend bool operator==(const DataflowTestState &,
                         const DataflowTestState &) = default;
};

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testCanonicalEvidenceSets() {
  using lang::optimization_detail::insertEvidenceId;
  using lang::optimization_detail::mergeEvidenceIds;
  using Id = std::uint64_t;
  // Include sparse, wide IDs: these are identities, not dense bit positions.
  const std::vector<Id> universe{
      1, 2, 17, 999, Id{1} << 40, std::numeric_limits<Id>::max()};
  const auto members = [&](unsigned mask) {
    std::vector<Id> ids;
    for (std::size_t index = 0; index < universe.size(); ++index) {
      if ((mask & (1U << index)) != 0) {
        ids.push_back(universe[index]);
      }
    }
    return ids;
  };
  for (unsigned left = 0; left < 64; ++left) {
    for (unsigned right = 0; right < 64; ++right) {
      auto actual = members(left);
      const auto incoming = members(right);
      const auto *data = actual.data();
      const auto capacity = actual.capacity();
      mergeEvidenceIds(actual, incoming);
      expect(actual == members(left | right),
             "evidence union retains every ID exactly once in canonical order");
      expect(incoming == members(right), "union preserves its source");
      if ((left | right) == left) {
        expect(actual.data() == data && actual.capacity() == capacity,
               "subset joins must retain the existing allocation");
      }
      mergeEvidenceIds(actual, actual);
      expect(actual == members(left | right), "self union is idempotent");
    }
    auto actual = members(left);
    insertEvidenceId(actual, Id{0});
    expect(actual == members(left), "zero does not become proof evidence");
    for (auto it = universe.rbegin(); it != universe.rend(); ++it) {
      insertEvidenceId(actual, *it);
      const auto *data = actual.data();
      insertEvidenceId(actual, *it);
      expect(actual.data() == data, "duplicate insertion does not allocate");
    }
    expect(actual == universe, "reverse insertion preserves canonical order");
  }
}

lang::MirBody schedulingBody() {
  lang::MirBody body;
  body.entry = 1;
  body.blocks = {
      {.id = 1,
       .terminator = {.kind = lang::MirTerminatorKind::Goto, .target = 2}},
      {.id = 2,
       .terminator = {.kind = lang::MirTerminatorKind::Branch,
                      .target = 2,
                      .elseTarget = 3}},
      {.id = 3, .terminator = {.kind = lang::MirTerminatorKind::Return}},
  };
  return body;
}

struct LegacyDataflowResult {
  std::vector<std::optional<DataflowTestState>> incoming;
  std::vector<lang::MirDataflowEdgeId> executableEdges;
  bool widened = false;
};

LegacyDataflowResult legacySchedulingSolve(const lang::MirBody &body) {
  LegacyDataflowResult result;
  result.incoming.resize(body.blocks.size());
  result.incoming[body.entry - 1] = DataflowTestState{};
  std::vector<std::size_t> updateCounts(body.blocks.size());
  std::vector<bool> queued(body.blocks.size(), false);
  std::deque<lang::MirBlockId> worklist{body.entry};
  queued[body.entry - 1] = true;

  while (!worklist.empty()) {
    const lang::MirBlockId source = worklist.front();
    worklist.pop_front();
    queued[source - 1] = false;
    const DataflowTestState incoming = *result.incoming[source - 1];
    std::vector<lang::MirForwardDataflowEdge<DataflowTestState>> edges;
    if (source == 1) {
      edges.push_back({.target = 2, .ordinal = 0, .state = incoming});
    } else if (source == 2) {
      edges.push_back({.target = 2,
                       .ordinal = 0,
                       .state = {.upper = std::min(8, incoming.upper + 1)}});
      edges.push_back({.target = 3, .ordinal = 1, .state = incoming});
    }
    for (const auto &edge : edges) {
      result.executableEdges.push_back(
          {.source = source, .target = edge.target, .ordinal = edge.ordinal});
      std::optional<DataflowTestState> &target =
          result.incoming[edge.target - 1];
      bool changed = false;
      if (!target) {
        target = edge.state;
        changed = true;
      } else {
        const int before = target->upper;
        if (updateCounts[edge.target - 1] >= 2 &&
            edge.state.upper > target->upper) {
          target->upper = 8;
          result.widened = true;
        } else {
          target->upper = std::max(target->upper, edge.state.upper);
        }
        changed = before != target->upper;
      }
      if (!changed) {
        continue;
      }
      ++updateCounts[edge.target - 1];
      if (!queued[edge.target - 1]) {
        worklist.push_back(edge.target);
        queued[edge.target - 1] = true;
      }
    }
  }
  std::sort(result.executableEdges.begin(), result.executableEdges.end());
  result.executableEdges.erase(
      std::unique(result.executableEdges.begin(), result.executableEdges.end()),
      result.executableEdges.end());
  return result;
}

void testForwardDataflowScheduling() {
  const lang::MirBody body = schedulingBody();
  bool widened = false;
  const auto solve = [&]() {
    return lang::runMirForwardDataflow(
        body, DataflowTestState{},
        [](const lang::MirBlock &block, const DataflowTestState &incoming) {
          std::vector<lang::MirForwardDataflowEdge<DataflowTestState>> edges;
          if (block.id == 1) {
            edges.push_back({.target = 2, .ordinal = 0, .state = incoming});
          } else if (block.id == 2) {
            edges.push_back(
                {.target = 2,
                 .ordinal = 0,
                 .state = {.upper = std::min(8, incoming.upper + 1)}});
            edges.push_back({.target = 3, .ordinal = 1, .state = incoming});
          }
          return edges;
        },
        [&](DataflowTestState &target, const DataflowTestState &incoming,
            lang::MirDataflowMergeContext context) {
          const int before = target.upper;
          if (context.widen && incoming.upper > target.upper) {
            target.upper = 8;
            widened = true;
          } else {
            target.upper = std::max(target.upper, incoming.upper);
          }
          return before != target.upper;
        },
        {.iterationLimit = 32, .widenAfterUpdates = 2});
  };
  const auto first = solve();
  const auto second = solve();
  const LegacyDataflowResult legacy = legacySchedulingSolve(body);
  expect(first.converged() && second.converged() &&
             first.incoming == second.incoming &&
             first.executableEdges == second.executableEdges &&
             first.incoming == legacy.incoming &&
             first.executableEdges == legacy.executableEdges && widened &&
             legacy.widened,
         "forward dataflow should be deterministic and invoke bounded "
         "widening while retaining the independent legacy FIFO result");
  expect(first.incoming.size() == 3 && first.incoming[1] && first.incoming[2] &&
             first.incoming[1]->upper == 8 && first.incoming[2]->upper == 8 &&
             first.executable({.source = 2, .target = 2, .ordinal = 0}) &&
             first.executable({.source = 2, .target = 3, .ordinal = 1}),
         "forward dataflow should retain the legacy FIFO fixed point and "
         "track executable edge identities");

  const auto duplicate = lang::runMirForwardDataflow(
      body, DataflowTestState{},
      [](const lang::MirBlock &block, const DataflowTestState &state) {
        if (block.id != 1) {
          return std::vector<lang::MirForwardDataflowEdge<DataflowTestState>>{};
        }
        return std::vector<lang::MirForwardDataflowEdge<DataflowTestState>>{
            {.target = 2, .ordinal = 0, .state = state},
            {.target = 2, .ordinal = 0, .state = state}};
      },
      [](DataflowTestState &, const DataflowTestState &,
         lang::MirDataflowMergeContext) { return false; });
  const auto bounded = lang::runMirForwardDataflow(
      body, DataflowTestState{},
      [](const lang::MirBlock &block, const DataflowTestState &state) {
        std::vector<lang::MirForwardDataflowEdge<DataflowTestState>> edges;
        if (block.id == 1) {
          edges.push_back({.target = 2, .ordinal = 0, .state = state});
        } else if (block.id == 2) {
          edges.push_back(
              {.target = 2, .ordinal = 0, .state = {.upper = state.upper + 1}});
        }
        return edges;
      },
      [](DataflowTestState &target, const DataflowTestState &incoming,
         lang::MirDataflowMergeContext) {
        if (incoming.upper <= target.upper) {
          return false;
        }
        target = incoming;
        return true;
      },
      {.iterationLimit = 4, .widenAfterUpdates = 0});
  const auto elseOnly = lang::runMirForwardDataflow(
      body, DataflowTestState{},
      [](const lang::MirBlock &block, const DataflowTestState &state) {
        if (block.id == 1) {
          return std::vector<lang::MirForwardDataflowEdge<DataflowTestState>>{
              {.target = 2, .ordinal = 0, .state = state}};
        }
        if (block.id == 2) {
          return std::vector<lang::MirForwardDataflowEdge<DataflowTestState>>{
              {.target = 3, .ordinal = 1, .state = state}};
        }
        return std::vector<lang::MirForwardDataflowEdge<DataflowTestState>>{};
      },
      [](DataflowTestState &, const DataflowTestState &,
         lang::MirDataflowMergeContext) { return false; });
  const auto invalidSlot = lang::runMirForwardDataflow(
      body, DataflowTestState{},
      [](const lang::MirBlock &block, const DataflowTestState &state) {
        if (block.id == 1) {
          return std::vector<lang::MirForwardDataflowEdge<DataflowTestState>>{
              {.target = 3, .ordinal = 0, .state = state}};
        }
        return std::vector<lang::MirForwardDataflowEdge<DataflowTestState>>{};
      },
      [](DataflowTestState &, const DataflowTestState &,
         lang::MirDataflowMergeContext) { return false; });
  lang::MirBody malformedArguments = body;
  malformedArguments.blocks[1].parameters.push_back(
      {.value = 1, .type = lang::SemanticType::Int32});
  const auto malformed = lang::runMirForwardDataflow(
      malformedArguments, DataflowTestState{},
      [](const lang::MirBlock &, const DataflowTestState &) {
        return std::vector<lang::MirForwardDataflowEdge<DataflowTestState>>{};
      },
      [](DataflowTestState &, const DataflowTestState &,
         lang::MirDataflowMergeContext) { return false; });
  const auto nonMonotone = lang::runMirForwardDataflow(
      body, DataflowTestState{},
      [](const lang::MirBlock &block, const DataflowTestState &state) {
        std::vector<lang::MirForwardDataflowEdge<DataflowTestState>> edges;
        if (block.id == 1) {
          edges.push_back({.target = 2, .ordinal = 0, .state = state});
        } else if (block.id == 2) {
          edges.push_back(
              {.target = 2, .ordinal = 0, .state = {.upper = state.upper + 1}});
          if (state.upper == 0) {
            edges.push_back({.target = 3, .ordinal = 1, .state = state});
          }
        }
        return edges;
      },
      [](DataflowTestState &target, const DataflowTestState &incoming,
         lang::MirDataflowMergeContext) {
        if (incoming.upper <= target.upper) {
          return false;
        }
        target = incoming;
        return true;
      });
  expect(duplicate.status == lang::MirDataflowStatus::InvalidControlFlow &&
             bounded.status == lang::MirDataflowStatus::IterationLimit,
         "forward dataflow should reject duplicate edge identities and stop "
         "a non-converging client at its explicit iteration bound");
  expect(elseOnly.converged() &&
             elseOnly.executable({.source = 2, .target = 3, .ordinal = 1}) &&
             !elseOnly.executable({.source = 2, .target = 2, .ordinal = 0}) &&
             invalidSlot.status ==
                 lang::MirDataflowStatus::InvalidControlFlow &&
             malformed.status == lang::MirDataflowStatus::InvalidControlFlow &&
             nonMonotone.status == lang::MirDataflowStatus::NonMonotoneTransfer,
         "forward dataflow should retain canonical terminator ordinals, "
         "reject malformed edge slots and block arguments, and fail closed "
         "when a client retracts an executable edge");
}

lang::MirOperand valueOperand(lang::MirValueId value, lang::SemanticType type) {
  return {.kind = lang::MirOperandKind::Value, .value = value, .type = type};
}

lang::MirInstruction literalInstruction(lang::MirInstructionId instruction,
                                        lang::MirValueId value,
                                        std::uint64_t literal) {
  return {.id = instruction,
          .kind = lang::MirInstructionKind::Compute,
          .result = value,
          .operation = lang::MirOperation::Literal,
          .literal = lang::Literal{literal},
          .literalProvenance = {.kind = lang::MirLiteralProvenanceKind::Source},
          .info = {.type = lang::SemanticType::UInt64}};
}

lang::MirBody affineLoopBody() {
  lang::MirBody body;
  body.entry = 1;
  body.returnType = lang::SemanticType::UInt64;
  body.values = {
      {.id = 1,
       .info = {.type = lang::SemanticType::UInt64},
       .definitionBlock = 2},
      {.id = 2,
       .info = {.type = lang::SemanticType::UInt64},
       .definitionBlock = 1,
       .definition = 1},
      {.id = 3,
       .info = {.type = lang::SemanticType::UInt64},
       .definitionBlock = 1,
       .definition = 2},
      {.id = 4,
       .info = {.type = lang::SemanticType::UInt64},
       .definitionBlock = 3,
       .definition = 5},
      {.id = 5,
       .info = {.type = lang::SemanticType::UInt64},
       .definitionBlock = 1,
       .definition = 3},
      {.id = 6,
       .info = {.type = lang::SemanticType::Bool},
       .definitionBlock = 2,
       .definition = 4},
      {.id = 7,
       .info = {.type = lang::SemanticType::UInt64},
       .definitionBlock = 2,
       .definition = 6},
  };
  body.blocks = {
      {.id = 1,
       .instructions = {literalInstruction(1, 2, 0),
                        literalInstruction(2, 3, 1),
                        literalInstruction(3, 5, 8)},
       .terminator = {.kind = lang::MirTerminatorKind::Goto,
                      .target = 2,
                      .targetArguments = {valueOperand(
                          2, lang::SemanticType::UInt64)}}},
      {.id = 2,
       .parameters = {{.value = 1, .type = lang::SemanticType::UInt64}},
       .instructions =
           {{.id = 6,
             .kind = lang::MirInstructionKind::Compute,
             .result = 7,
             .operands = {valueOperand(3, lang::SemanticType::UInt64)},
             .operation = lang::MirOperation::Identity,
             .info = {.type = lang::SemanticType::UInt64}},
            {.id = 4,
             .kind = lang::MirInstructionKind::Compute,
             .result = 6,
             .operands = {valueOperand(1, lang::SemanticType::UInt64),
                          valueOperand(5, lang::SemanticType::UInt64)},
             .operation = lang::MirOperation::Less,
             .info = {.type = lang::SemanticType::Bool}}},
       .terminator = {.kind = lang::MirTerminatorKind::Branch,
                      .value = valueOperand(6, lang::SemanticType::Bool),
                      .target = 3,
                      .elseTarget = 4}},
      {.id = 3,
       .instructions =
           {{.id = 5,
             .kind = lang::MirInstructionKind::Compute,
             .result = 4,
             .operands = {valueOperand(1, lang::SemanticType::UInt64),
                          valueOperand(3, lang::SemanticType::UInt64)},
             .operation = lang::MirOperation::Add,
             .info = {.type = lang::SemanticType::UInt64}}},
       .terminator = {.kind = lang::MirTerminatorKind::Goto,
                      .target = 2,
                      .targetArguments = {valueOperand(
                          4, lang::SemanticType::UInt64)}}},
      {.id = 4,
       .terminator = {.kind = lang::MirTerminatorKind::Return,
                      .value = valueOperand(1, lang::SemanticType::UInt64)}},
  };
  return body;
}

void testNaturalLoopAndInduction() {
  const lang::MirBody body = affineLoopBody();
  const std::optional<lang::MirDominanceInfo> dominance =
      lang::computeMirDominance(body);
  const lang::MirLoopAnalysis loops =
      dominance ? lang::analyzeMirLoops(body, *dominance)
                : lang::MirLoopAnalysis{};
  const lang::MirNaturalLoop *loop = loops.loopForHeader(2);
  expect(dominance && !loops.hasIrreducibleControlFlow && loop != nullptr &&
             loop->blocks == std::vector<lang::MirBlockId>{2, 3} &&
             loop->latches == std::vector<lang::MirBlockId>{3} &&
             loop->invariantValues == std::vector<lang::MirValueId>{7} &&
             loop->inductions.size() == 1,
         "loop analysis should derive the natural loop and its single latch");
  if (loop != nullptr && loop->inductions.size() == 1) {
    const lang::MirAffineInductionFact &induction = loop->inductions.front();
    expect(induction.parameter == 1 && induction.stepMagnitude == 1 &&
               !induction.decreasing && induction.range &&
               induction.range->relation == lang::MirOperation::Less &&
               induction.range->constrainedSuccessor == 3,
           "loop analysis should recognize a bounded affine block-parameter "
           "induction and its relational continuation fact");
  }

  lang::MirBody ambiguous = affineLoopBody();
  ambiguous.values.push_back({.id = 8,
                              .info = {.type = lang::SemanticType::Bool},
                              .definitionBlock = 1,
                              .definition = 7});
  ambiguous.blocks[0].instructions.push_back(
      {.id = 7,
       .kind = lang::MirInstructionKind::Compute,
       .result = 8,
       .operation = lang::MirOperation::Literal,
       .literal = lang::Literal{true},
       .literalProvenance = {.kind = lang::MirLiteralProvenanceKind::Source},
       .info = {.type = lang::SemanticType::Bool}});
  ambiguous.blocks[0].terminator.kind = lang::MirTerminatorKind::Branch;
  ambiguous.blocks[0].terminator.value =
      valueOperand(8, lang::SemanticType::Bool);
  ambiguous.blocks[0].terminator.elseTarget = 2;
  ambiguous.blocks[0].terminator.elseArguments = {
      valueOperand(3, lang::SemanticType::UInt64)};
  const std::optional<lang::MirDominanceInfo> ambiguousDominance =
      lang::computeMirDominance(ambiguous);
  const lang::MirLoopAnalysis ambiguousLoops =
      ambiguousDominance ? lang::analyzeMirLoops(ambiguous, *ambiguousDominance)
                         : lang::MirLoopAnalysis{};
  const lang::MirNaturalLoop *ambiguousLoop = ambiguousLoops.loopForHeader(2);
  expect(ambiguousLoop != nullptr && ambiguousLoop->inductions.empty(),
         "loop analysis should reject an induction when repeated incoming "
         "CFG edges carry ambiguous block arguments");
}

lang::MirBody nestedLoopBody() {
  lang::MirBody body;
  body.entry = 1;
  body.blocks = {
      {.id = 1,
       .terminator = {.kind = lang::MirTerminatorKind::Goto, .target = 2}},
      {.id = 2,
       .terminator = {.kind = lang::MirTerminatorKind::Branch,
                      .target = 3,
                      .elseTarget = 7}},
      {.id = 3,
       .terminator = {.kind = lang::MirTerminatorKind::Goto, .target = 4}},
      {.id = 4,
       .terminator = {.kind = lang::MirTerminatorKind::Branch,
                      .target = 5,
                      .elseTarget = 6}},
      {.id = 5,
       .terminator = {.kind = lang::MirTerminatorKind::Goto, .target = 4}},
      {.id = 6,
       .terminator = {.kind = lang::MirTerminatorKind::Goto, .target = 2}},
      {.id = 7, .terminator = {.kind = lang::MirTerminatorKind::Return}},
  };
  return body;
}

lang::MirBody irreducibleBody() {
  lang::MirBody body;
  body.entry = 1;
  body.blocks = {
      {.id = 1,
       .terminator = {.kind = lang::MirTerminatorKind::Branch,
                      .target = 2,
                      .elseTarget = 3}},
      {.id = 2,
       .terminator = {.kind = lang::MirTerminatorKind::Goto, .target = 4}},
      {.id = 3,
       .terminator = {.kind = lang::MirTerminatorKind::Goto, .target = 4}},
      {.id = 4,
       .terminator = {.kind = lang::MirTerminatorKind::Branch,
                      .target = 2,
                      .elseTarget = 5}},
      {.id = 5, .terminator = {.kind = lang::MirTerminatorKind::Return}},
  };
  return body;
}

void testLoopForestAndIrreducibleControl() {
  const lang::MirBody nested = nestedLoopBody();
  const std::optional<lang::MirDominanceInfo> nestedDominance =
      lang::computeMirDominance(nested);
  const lang::MirLoopAnalysis forest =
      nestedDominance ? lang::analyzeMirLoops(nested, *nestedDominance)
                      : lang::MirLoopAnalysis{};
  const lang::MirNaturalLoop *outer = forest.loopForHeader(2);
  const lang::MirNaturalLoop *inner = forest.loopForHeader(4);
  expect(outer != nullptr && inner != nullptr && forest.loops.size() == 2 &&
             !outer->parent && outer->children == std::vector<std::size_t>{1} &&
             outer->depth == 0 && inner->parent == 0 && inner->depth == 1,
         "loop analysis should build a deterministic strict-containment "
         "forest for nested natural loops");

  const lang::MirBody irreducible = irreducibleBody();
  const std::optional<lang::MirDominanceInfo> irreducibleDominance =
      lang::computeMirDominance(irreducible);
  const lang::MirLoopAnalysis irreducibleAnalysis =
      irreducibleDominance
          ? lang::analyzeMirLoops(irreducible, *irreducibleDominance)
          : lang::MirLoopAnalysis{};
  expect(irreducibleAnalysis.hasIrreducibleControlFlow,
         "loop analysis should mark a multi-entry strongly connected region "
         "instead of manufacturing a natural-loop proof");
}

lang::MirPlace bindingPlace(lang::MirPlaceId id, lang::HirBindingId binding) {
  return {
      .id = id, .root = lang::MirPlaceRootKind::Binding, .binding = binding};
}

void testPlaceRelations() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("optimizer-alias-fields.gti", R"(
struct Pair {
  int32_t left = 0;
  int32_t right = 0;

  Pair(int32_t first, int32_t second) : left(first), right(second) {}
};

union Overlay {
  mut uint64_t first;
  mut uint64_t second;
};

int main() {
  Pair pair{1, 2};
  mut Overlay overlay{uint64_t(0)};
  unsafe {
    overlay.second = uint64_t(1);
  }
  return pair.left;
}
)");
  const lang::MirClassInstance *record = nullptr;
  const lang::MirClassInstance *overlay = nullptr;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    if (instance.kind == lang::ClassKind::Struct) {
      record = &instance;
    } else if (instance.kind == lang::ClassKind::Union) {
      overlay = &instance;
    }
  }
  expect(frontend.canGenerateCode() && record != nullptr &&
             record->declaredFields.size() == 2 && overlay != nullptr &&
             overlay->declaredFields.size() == 2,
         "the alias fixture should provide verified record and union field "
         "metadata");
  if (!frontend.canGenerateCode() || record == nullptr ||
      record->declaredFields.size() != 2 || overlay == nullptr ||
      overlay->declaredFields.size() != 2) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    std::cerr << "MIR class instances: " << frontend.mir.classInstances().size()
              << '\n';
    return;
  }

  lang::MirPlace root = bindingPlace(1, 1);
  lang::MirPlace same = bindingPlace(2, 1);
  lang::MirPlace other = bindingPlace(3, 2);
  lang::MirPlace leftField = bindingPlace(4, 1);
  leftField.projections.push_back({.kind = lang::MirProjectionKind::Field,
                                   .field = record->declaredFields[0].symbol});
  lang::MirPlace rightField = bindingPlace(5, 1);
  rightField.projections.push_back({.kind = lang::MirProjectionKind::Field,
                                    .field = record->declaredFields[1].symbol});
  lang::MirPlace unionLeft = bindingPlace(15, 1);
  unionLeft.projections.push_back({.kind = lang::MirProjectionKind::Field,
                                   .field = overlay->declaredFields[0].symbol});
  lang::MirPlace unionRight = bindingPlace(16, 1);
  unionRight.projections.push_back(
      {.kind = lang::MirProjectionKind::Field,
       .field = overlay->declaredFields[1].symbol});
  lang::MirPlace dynamicLeft = bindingPlace(6, 1);
  dynamicLeft.projections.push_back(
      {.kind = lang::MirProjectionKind::Index, .index = 1});
  lang::MirPlace dynamicRight = bindingPlace(7, 1);
  dynamicRight.projections.push_back(
      {.kind = lang::MirProjectionKind::Index, .index = 2});
  lang::MirPlace raw = bindingPlace(8, 2);
  raw.projections.push_back({.kind = lang::MirProjectionKind::RawDereference});
  lang::MirPlace loan = {
      .id = 9, .root = lang::MirPlaceRootKind::Loan, .loan = 1};
  lang::MirPlace value = {
      .id = 10, .root = lang::MirPlaceRootKind::Value, .value = 1};
  lang::MirPlace temporary = {
      .id = 11, .root = lang::MirPlaceRootKind::Temporary, .temporary = 1};
  lang::MirPlace otherTemporary = {
      .id = 12, .root = lang::MirPlaceRootKind::Temporary, .temporary = 2};
  lang::MirPlace constantLeft = bindingPlace(13, 1);
  constantLeft.projections.push_back(
      {.kind = lang::MirProjectionKind::Index, .constantIndex = 0});
  lang::MirPlace constantRight = bindingPlace(14, 1);
  constantRight.projections.push_back(
      {.kind = lang::MirProjectionKind::Index, .constantIndex = 1});
  lang::MirPlace receiver = {.id = 17, .root = lang::MirPlaceRootKind::This};
  receiver.projections.push_back({.kind = lang::MirProjectionKind::Field,
                                  .field = record->declaredFields[0].symbol});
  lang::MirPlace symbol = {
      .id = 18, .root = lang::MirPlaceRootKind::Symbol, .symbol = 1000};

  const auto relation = [&](const lang::MirPlace &left,
                            const lang::MirPlace &right) {
    return lang::mirPlaceRelation(frontend.mir, left, right);
  };
  expect(
      relation(root, same) == lang::MirAliasRelation::MustAlias &&
          relation(root, other) == lang::MirAliasRelation::NoAlias &&
          relation(leftField, rightField) == lang::MirAliasRelation::NoAlias &&
          relation(unionLeft, unionRight) == lang::MirAliasRelation::MayAlias &&
          relation(dynamicLeft, dynamicRight) ==
              lang::MirAliasRelation::MayAlias &&
          relation(other, raw) == lang::MirAliasRelation::MayAlias &&
          relation(root, loan) == lang::MirAliasRelation::MayAlias &&
          relation(root, value) == lang::MirAliasRelation::MayAlias &&
          relation(temporary, otherTemporary) ==
              lang::MirAliasRelation::NoAlias &&
          relation(constantLeft, constantRight) ==
              lang::MirAliasRelation::NoAlias &&
          relation(root, receiver) == lang::MirAliasRelation::NoAlias &&
          relation(root, symbol) == lang::MirAliasRelation::NoAlias &&
          relation(receiver, symbol) == lang::MirAliasRelation::NoAlias,
      "place analysis should distinguish exact roots and disjoint record "
      "fields, receiver fields, globals, and local bindings while keeping "
      "union fields, borrowed, value-rooted, dynamic, and raw memory "
      "conservative");
  const std::vector<const lang::MirPlace *> places = {
      &root,          &same,       &other,       &leftField,      &rightField,
      &unionLeft,     &unionRight, &dynamicLeft, &dynamicRight,   &raw,
      &loan,          &value,      &temporary,   &otherTemporary, &constantLeft,
      &constantRight, &receiver,   &symbol};
  bool symmetric = true;
  for (const lang::MirPlace *left : places) {
    for (const lang::MirPlace *right : places) {
      symmetric =
          symmetric && relation(*left, *right) == relation(*right, *left);
    }
  }
  expect(symmetric, "place alias relations should be symmetric");
}

const lang::MirFunctionInstance *
findMirFunction(const lang::FrontendResult &frontend, std::string_view name) {
  const auto found =
      std::find_if(frontend.mir.functionInstances().begin(),
                   frontend.mir.functionInstances().end(),
                   [&](const lang::MirFunctionInstance &function) {
                     const lang::HirFunctionInstance *hir =
                         frontend.hir.findFunctionInstance(function.id);
                     return hir != nullptr && hir->source != nullptr &&
                            hir->source->name().lexeme == name;
                   });
  return found == frontend.mir.functionInstances().end() ? nullptr : &*found;
}

void testExactCallEscapeAnalysis() {
  const std::string source = R"(
class Box {
public:
  mut int value = 1;
  mut int& get() mut { return this.value; }
};

int& relay(int& source) { return source; }

int use_receiver_relay() {
  mut Box box = Box();
  mut int& alias = box.get();
  return alias;
}

int use_argument_relay() {
  int value = 1;
  int& alias = relay(value);
  return alias;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("optimizer-alias-escape.gti", source);
  const lang::MirFunctionInstance *receiver =
      findMirFunction(frontend, "use_receiver_relay");
  const lang::MirFunctionInstance *argument =
      findMirFunction(frontend, "use_argument_relay");
  expect(frontend.canGenerateCode() && receiver != nullptr &&
             argument != nullptr,
         "alias escape fixture should pass the frontend");
  if (!frontend.canGenerateCode() || receiver == nullptr ||
      argument == nullptr) {
    return;
  }
  const auto hasExactEscape = [&](const lang::MirFunctionInstance &function) {
    const lang::MirPlaceAliasEscapeAnalysis analysis =
        lang::analyzeMirPlaceAliasEscape(
            frontend.mir,
            {.kind = lang::MirBodyKind::Function, .owner = function.id});
    return std::any_of(analysis.escapes.begin(), analysis.escapes.end(),
                       [](const lang::MirPlaceEscapeFact &fact) {
                         return fact.has(lang::MirPlaceEscapeReason::ExactCall);
                       });
  };
  expect(hasExactEscape(*receiver) && hasExactEscape(*argument),
         "escape analysis should project verified receiver and argument "
         "escape summaries onto the caller's originating MIR places");
}

void testUnknownCallEscapeAnalysis() {
  const std::string source = R"(
extern "C" {
  void opaque(int32_t* value);
}

int32_t unknown_escape() {
  mut int32_t value = 0;
  unsafe {
    opaque(&value);
  }
  return value;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("optimizer-alias-unknown.gti", source);
  const lang::MirFunctionInstance *function =
      findMirFunction(frontend, "unknown_escape");
  expect(frontend.canGenerateCode() && function != nullptr,
         "unknown-call escape fixture should pass the frontend");
  if (!frontend.canGenerateCode() || function == nullptr) {
    return;
  }
  const lang::MirPlaceAliasEscapeAnalysis analysis =
      lang::analyzeMirPlaceAliasEscape(
          frontend.mir,
          {.kind = lang::MirBodyKind::Function, .owner = function->id});
  expect(std::any_of(analysis.escapes.begin(), analysis.escapes.end(),
                     [](const lang::MirPlaceEscapeFact &fact) {
                       return fact.has(lang::MirPlaceEscapeReason::UnknownCall);
                     }),
         "native and unresolved call boundaries should conservatively mark "
         "originating places as escaping");
}

void testAliasAwareSafetyInvalidation() {
  const std::string source = R"(
class Pair {
public:
  mut int index = 0;
  mut int other = 0;
};

int disjoint_field_read() {
  mut Pair pair = Pair();
  pair.index = 1;
  pair.other = 9;
  int values[2] = {4, 5};
  return values[pair.index];
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("optimizer-alias-safety.gti", source);
  const lang::MirFunctionInstance *function =
      findMirFunction(frontend, "disjoint_field_read");
  expect(frontend.canGenerateCode() && function != nullptr,
         "alias-aware safety fixture should pass the frontend");
  if (!frontend.canGenerateCode() || function == nullptr) {
    return;
  }
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = function->id};
  const lang::MirSafetyAnalysis safety =
      lang::analyzeMirSafetyChecks(frontend.mir, address);
  const bool provedBounds = std::any_of(
      safety.proofs.begin(), safety.proofs.end(),
      [](const lang::MirSafetyProof &proof) {
        return proof.discharge.family == lang::MirSafetyCheckFamily::Bounds;
      });
  expect(provedBounds,
         "a write to a proved-disjoint field should preserve the tracked "
         "index range used by a later fixed-array bounds proof");
}

void testConservativeSafetyEdgesAndUnionOverlap() {
  const std::string source = R"(
union Overlay {
  mut uint64_t index;
  mut uint64_t replacement;
};

int32_t default_checked(int32_t selector) {
  switch (selector) {
  case 1:
    return 0;
  default:
    int32_t values[1] = {7};
    return values[uint64_t(0)];
  }
}

int32_t union_overlap_read() {
  mut Overlay overlay{uint64_t(0)};
  int32_t values[1] = {7};
  unsafe {
    overlay.index = uint64_t(0);
    overlay.replacement = uint64_t(2);
    return values[overlay.index];
  }
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("optimizer-safety-edges.gti", source);
  const lang::MirFunctionInstance *defaultChecked =
      findMirFunction(frontend, "default_checked");
  const lang::MirFunctionInstance *unionRead =
      findMirFunction(frontend, "union_overlap_read");
  expect(frontend.canGenerateCode() && defaultChecked != nullptr &&
             unionRead != nullptr,
         "switch-default and union-overlap safety fixtures should pass the "
         "frontend");
  if (!frontend.canGenerateCode() || defaultChecked == nullptr ||
      unionRead == nullptr) {
    return;
  }
  const auto safetyFor = [&](const lang::MirFunctionInstance &function) {
    return lang::analyzeMirSafetyChecks(
        frontend.mir,
        {.kind = lang::MirBodyKind::Function, .owner = function.id});
  };
  const auto hasBoundsProof = [](const lang::MirSafetyAnalysis &analysis) {
    return std::any_of(analysis.proofs.begin(), analysis.proofs.end(),
                       [](const lang::MirSafetyProof &proof) {
                         return proof.discharge.family ==
                                lang::MirSafetyCheckFamily::Bounds;
                       });
  };
  expect(hasBoundsProof(safetyFor(*defaultChecked)),
         "safety analysis should propagate state along a switch default edge");
  expect(!hasBoundsProof(safetyFor(*unionRead)),
         "a write through an overlapping union field should invalidate a "
         "previous fact instead of discharging a later bounds check");
}

void testSafetyInvokeSnapshot() {
  using namespace lang;
  const auto frontend = Frontend().analyze("invoke-state.gti", R"(
int32_t read_after_add() {
  int32_t values[2] = {7, 8};
  mut uint64_t index = 0;
  uint64_t next = index + 1;
  return values[next];
}
)");
  const auto *function = findMirFunction(frontend, "read_after_add");
  expect(frontend.canGenerateCode() && function,
         "invoke snapshot fixture lowers");
  if (!frontend.canGenerateCode() || !function)
    return;
  const MirBodyAddress address{.kind = MirBodyKind::Function,
                               .owner = function->id};
  const auto hasBounds = [](const MirSafetyAnalysis &analysis) {
    return std::any_of(analysis.proofs.begin(), analysis.proofs.end(),
                       [](const MirSafetyProof &proof) {
                         return proof.discharge.family ==
                                MirSafetyCheckFamily::Bounds;
                       });
  };
  expect(hasBounds(analyzeMirSafetyChecks(frontend.mir, address)),
         "successful addition publishes the in-bounds index fact");

  // Analysis-only CFG probe: route the failure edge to the read so its state
  // is observable. Production failure cleanup normally terminates instead.
  // Swap both edges, retaining the same CFG shape and stable identities.
  auto failurePath = frontend.mir;
  auto *body = findMirBody(failurePath, address);
  bool swapped = false;
  for (auto &block : body->blocks) {
    if (block.terminator.kind != MirTerminatorKind::Invoke)
      continue;
    const auto instruction =
        std::find_if(block.instructions.begin(), block.instructions.end(),
                     [&](const auto &i) {
                       return i.id == block.terminator.invokeInstruction &&
                              i.kind == MirInstructionKind::Compute &&
                              i.operation == MirOperation::Add;
                     });
    if (instruction == block.instructions.end())
      continue;
    std::swap(block.terminator.target, block.terminator.elseTarget);
    std::swap(block.terminator.targetArguments, block.terminator.elseArguments);
    swapped = true;
    break;
  }
  expect(swapped, "probe locates the checked addition Invoke");
  expect(swapped && !hasBounds(analyzeMirSafetyChecks(failurePath, address)),
         "failure edge cannot inherit the successful addition's result fact");
}

void testSparseConditionalConstants() {
  const std::string source = R"(
int32_t selected(bool choose) {
  mut int32_t value = 0;
  if (choose) {
    value = 7;
  } else {
    value = 7;
  }
  if (value == 7) {
    return 1;
  }
  return 0;
}

int32_t main() { return selected(true) == 1 ? 0 : 1; }
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("optimizer-sccp.gti", source);
  expect(frontend.canGenerateCode(), "SCCP fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O2});
  const lang::OptimizedProgram repeated =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O2});
  const bool sparseProof = std::any_of(
      optimized.report.transformations.begin(),
      optimized.report.transformations.end(),
      [](const lang::MirTransformRecord &record) {
        return record.proof == lang::MirTransformProofKind::SparseConstant;
      });
  const std::string report =
      lang::serializeOptimizationReportJson(optimized.report, "O2");
  expect(
      optimized.valid() && repeated.valid() && optimized.mir == repeated.mir &&
          optimized.report.transformations == repeated.report.transformations &&
          sparseProof &&
          report.find("\"sparseConstants\":true") != std::string::npos,
      "O2 SCCP should replace a value proven constant only after equal "
      "executable-edge arguments meet, with replayable sparse evidence "
      "recorded in the versioned report");

  std::vector<lang::MirTransformRecord> forged =
      optimized.report.transformations;
  const auto sparse =
      std::find_if(forged.begin(), forged.end(), [](const auto &record) {
        return record.proof == lang::MirTransformProofKind::SparseConstant;
      });
  if (sparse != forged.end()) {
    sparse->relatedValues.push_back(
        std::numeric_limits<lang::MirValueId>::max());
    std::sort(forged.begin(), forged.end(), lang::mirTransformRecordLess);
  }
  expect(sparse != forged.end() && !lang::verifyMirOptimizationCoherence(
                                        frontend.mir, optimized.mir, forged)
                                        .valid(),
         "SCCP coherence replay should reject forged sparse evidence");
}

} // namespace

int main() {
  testForwardDataflowScheduling();
  testCanonicalEvidenceSets();
  testNaturalLoopAndInduction();
  testLoopForestAndIrreducibleControl();
  testPlaceRelations();
  testExactCallEscapeAnalysis();
  testUnknownCallEscapeAnalysis();
  testAliasAwareSafetyInvalidation();
  testConservativeSafetyEdgesAndUnionOverlap();
  testSparseConditionalConstants();
  testSafetyInvokeSnapshot();
  if (failures != 0) {
    std::cerr << failures << " optimizer analysis test(s) failed\n";
    return 1;
  }
  std::cout << "optimizer analysis tests passed\n";
  return 0;
}
