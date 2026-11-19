// Whole-program reuse must agree with fresh lowering when callees or cleanup
// paths change.
#include "../../src/compiler/mir_verification_session.h"
#include "gti/frontend.h"
#include "gti/mir_lowering.h"
#include "unit_test.h"

#include <iostream>

GTI_UNIT_TEST(incremental_mir_verification) {
  using namespace lang;
  FrontendWorkMetrics work;
  FrontendOptions options;
  options.workMetrics = &work;
  options.mirVerificationCache = std::make_shared<MirVerificationCache>();
  // The same caller and middle function reach a newly fallible leaf. Check
  // the complete MIR (including loan ends, invoke edges and all summaries)
  // against cold lowering of the very same concrete HIR on every revision.
  for (const auto &leaf : {"return n;", "return 18 / n;", "return n;"}) {
    const auto source = std::string("int leaf(int n) { ") + leaf +
                        " } int middle(int n) { return leaf(n); } "
                        "int main() { return middle(3); }";
    const auto compiled = Frontend(options).analyze("reuse.gti", source);
    if (!compiled.mirValid || !compiled.diagnostics.empty() ||
        !work.mirRoundBodyReuses || !work.mirRoundBodyLowerings ||
        !work.mirDominanceReuses) {
      for (const auto &error : compiled.diagnostics)
        std::cerr << error.message << '\n';
      std::cerr
          << "Expected fully verified MIR with both fresh and reused bodies.\n";
      return 1;
    }
    for (const auto &instance : compiled.hir.functionInstances()) {
      if (!instance.source || instance.source->name().lexeme != "middle")
        continue;
      const auto *caller = compiled.mir.findFunctionInstance(instance.id);
      if (!caller || caller->mayRaiseDefinedFailure !=
                         (std::string_view(leaf) == "return 18 / n;")) {
        std::cerr << "Changed leaf effect did not reach middle: " << leaf
                  << '\n';
        return 1;
      }
    }
    auto fresh = MirLowerer().lower(compiled.hir, compiled.failureMetadata);
    if (!fresh.valid() || !(fresh.program == compiled.mir)) {
      std::cerr << "Reused lowering differs from the clean fixed point after: "
                << leaf << '\n';
      return 1;
    }
  }
  return 0;
}

GTI_UNIT_TEST(incremental_mir_boundary_inputs) {
  using namespace lang;
  MirVerificationCache cache;
  FrontendWorkMetrics work;
  mir_detail::VerificationSession session(&cache, &work);
  MirBody body;
  body.entry = 1;
  body.fullExpressions = {{.id = 1, .roots = {7}}};
  body.blocks = {
      {.id = 1,
       .terminator = {.kind = MirTerminatorKind::Branch,
                      .target = 2,
                      .elseTarget = 3}},
      {.id = 2,
       .instructions = {{.id = 1, .fullExpressionEnd = 1}},
       .terminator = {.kind = MirTerminatorKind::Goto, .target = 3}},
      {.id = 3,
       .instructions = {{.id = 2, .hirValue = 7},
                        {.id = 3, .fullExpressionEnd = 1}},
       .terminator = {.kind = MirTerminatorKind::Return}},
  };
  const auto conflict = [&]() {
    auto dom = computeMirDominance(body, &session);
    return mir_detail::conflictingExpressionBoundaries(body, {2}, *dom,
                                                       &session);
  };
  if (conflict() || conflict() || work.mirBoundaryReuses != 1)
    return 1;
  // Identical edges and markers. Moving the restart to after the marker
  // makes both markers reachable in one evaluation.
  std::swap(body.blocks[2].instructions[0], body.blocks[2].instructions[1]);
  if (conflict() != std::pair<MirBlockId, MirInstructionId>{3, 3} ||
      work.mirBoundaryReuses != 1)
    return 1;
  // Now it is the edge alone that makes the two markers exclusive.
  body.blocks[1].terminator = {.kind = MirTerminatorKind::Return};
  if (conflict() || conflict() || work.mirBoundaryReuses != 2)
    return 1;
  body.blocks[1].terminator = {.kind = MirTerminatorKind::Goto, .target = 3};
  if (!conflict())
    return 1;
  // A malformed target can never hit a previously verified dominance tree.
  body.blocks[1].terminator.target = 12;
  return computeMirDominance(body, &session) ? 1 : 0;
}
