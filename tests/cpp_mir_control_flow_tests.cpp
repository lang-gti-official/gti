#include "../src/compiler/cpp_mir_control_flow.h"

#include <algorithm>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool hasIssue(const lang::CppMirControlFlowVerification &verification,
              lang::CppMirControlFlowIssueKind kind) {
  return std::any_of(verification.issues.begin(), verification.issues.end(),
                     [kind](const lang::CppMirControlFlowIssue &issue) {
                       return issue.kind == kind;
                     });
}

lang::MirBlock terminal(lang::MirBlockId id, lang::MirTerminatorKind kind) {
  return {.id = id, .terminator = {.kind = kind}};
}

lang::MirBody representativeBody() {
  lang::MirBody body;
  body.entry = 1;

  lang::MirBlock branch = terminal(1, lang::MirTerminatorKind::Branch);
  branch.terminator.target = 2;
  branch.terminator.elseTarget = 3;

  lang::MirBlock selection = terminal(2, lang::MirTerminatorKind::Switch);
  selection.terminator.target = 4;
  selection.terminator.switchTargets = {
      {.target = 3},
      // Repeated targets must remain distinct through their canonical slots.
      {.target = 3},
  };

  lang::MirBlock invoke = terminal(3, lang::MirTerminatorKind::Invoke);
  invoke.terminator.target = 4;
  invoke.terminator.elseTarget = 5;

  lang::MirBlock transfer = terminal(4, lang::MirTerminatorKind::Goto);
  transfer.terminator.target = 6;

  body.blocks = {
      std::move(branch),
      std::move(selection),
      std::move(invoke),
      std::move(transfer),
      terminal(5, lang::MirTerminatorKind::Unreachable),
      terminal(6, lang::MirTerminatorKind::Return),
      terminal(7, lang::MirTerminatorKind::Exit),
      terminal(8, lang::MirTerminatorKind::PropagateFailure),
      terminal(9, lang::MirTerminatorKind::ContainFailure),
      terminal(10, lang::MirTerminatorKind::TerminateCleanupFailure),
  };
  return body;
}

void testDeterministicExactCoverage() {
  const lang::MirBody body = representativeBody();
  const lang::CppMirControlFlowPlan first =
      lang::buildCppMirControlFlowPlan(body);
  const lang::CppMirControlFlowPlan second =
      lang::buildCppMirControlFlowPlan(body);

  expect(first == second, "the same MIR body should produce the same plan");
  expect(lang::verifyCppMirControlFlowPlan(body, first).valid(),
         "the canonical plan should exactly cover representative MIR");
  expect(first.entry == 1 && first.blocks.size() == body.blocks.size(),
         "the plan should copy the entry and every block");
  expect(first.blocks[0].label == "__gti_mir_bb_1" &&
             first.blocks[9].label == "__gti_mir_bb_10",
         "block labels should be stable and derived only from block IDs");
  expect(first.blocks[0].outgoing ==
             std::vector<lang::CppMirControlFlowEdge>{
                 {.target = 2, .ordinal = 0}, {.target = 3, .ordinal = 1}},
         "branch edges should retain their canonical true/false slots");
  expect(
      first.blocks[1].outgoing ==
          std::vector<lang::CppMirControlFlowEdge>{{.target = 4, .ordinal = 0},
                                                   {.target = 3, .ordinal = 1},
                                                   {.target = 3, .ordinal = 2}},
      "switch default and repeated-target cases should remain distinct");
  expect(first.blocks[2].outgoing ==
             std::vector<lang::CppMirControlFlowEdge>{
                 {.target = 4, .ordinal = 0}, {.target = 5, .ordinal = 1}},
         "invoke success and failure edges should both be represented");
  expect(std::all_of(first.blocks.begin() + 4, first.blocks.end(),
                     [](const lang::CppMirControlFlowBlock &block) {
                       return block.outgoing.empty();
                     }),
         "terminal MIR vocabulary should not invent successor edges");
}

void testMalformedPlansFailClosed() {
  const lang::MirBody body = representativeBody();
  const lang::CppMirControlFlowPlan canonical =
      lang::buildCppMirControlFlowPlan(body);

  lang::CppMirControlFlowPlan wrongEntry = canonical;
  wrongEntry.entry = 2;
  expect(hasIssue(lang::verifyCppMirControlFlowPlan(body, wrongEntry),
                  lang::CppMirControlFlowIssueKind::EntryCoverage),
         "a changed entry should fail entry coverage");

  lang::CppMirControlFlowPlan missingBlock = canonical;
  missingBlock.blocks.pop_back();
  expect(hasIssue(lang::verifyCppMirControlFlowPlan(body, missingBlock),
                  lang::CppMirControlFlowIssueKind::BlockCoverage),
         "a missing block should fail exact block coverage");

  lang::CppMirControlFlowPlan reorderedBlocks = canonical;
  std::swap(reorderedBlocks.blocks[0], reorderedBlocks.blocks[1]);
  expect(hasIssue(lang::verifyCppMirControlFlowPlan(body, reorderedBlocks),
                  lang::CppMirControlFlowIssueKind::BlockCoverage),
         "reordered plan blocks should fail deterministic coverage");

  lang::CppMirControlFlowPlan changedLabel = canonical;
  changedLabel.blocks[0].label = "not_the_canonical_label";
  expect(hasIssue(lang::verifyCppMirControlFlowPlan(body, changedLabel),
                  lang::CppMirControlFlowIssueKind::LabelCoverage),
         "a changed target label should fail label coverage");

  lang::CppMirControlFlowPlan missingEdge = canonical;
  missingEdge.blocks[1].outgoing.pop_back();
  expect(hasIssue(lang::verifyCppMirControlFlowPlan(body, missingEdge),
                  lang::CppMirControlFlowIssueKind::EdgeCoverage),
         "a missing switch edge should fail exact edge coverage");

  lang::CppMirControlFlowPlan duplicateEdge = canonical;
  duplicateEdge.blocks[1].outgoing.push_back(
      duplicateEdge.blocks[1].outgoing.back());
  expect(hasIssue(lang::verifyCppMirControlFlowPlan(body, duplicateEdge),
                  lang::CppMirControlFlowIssueKind::EdgeCoverage),
         "a duplicate switch edge should fail exact edge coverage");

  lang::CppMirControlFlowPlan changedTarget = canonical;
  changedTarget.blocks[2].outgoing[1].target = 6;
  expect(hasIssue(lang::verifyCppMirControlFlowPlan(body, changedTarget),
                  lang::CppMirControlFlowIssueKind::EdgeCoverage),
         "an invoke edge retarget should fail exact edge coverage");

  lang::CppMirControlFlowPlan changedOrdinal = canonical;
  changedOrdinal.blocks[1].outgoing[2].ordinal = 1;
  expect(hasIssue(lang::verifyCppMirControlFlowPlan(body, changedOrdinal),
                  lang::CppMirControlFlowIssueKind::EdgeCoverage),
         "a repeated-target switch edge with the wrong slot should fail");
}

void testMalformedMirFailsClosed() {
  lang::MirBody body = representativeBody();
  body.blocks[0].terminator.kind = lang::MirTerminatorKind::None;
  const lang::CppMirControlFlowPlan plan =
      lang::buildCppMirControlFlowPlan(body);
  expect(hasIssue(lang::verifyCppMirControlFlowPlan(body, plan),
                  lang::CppMirControlFlowIssueKind::InvalidMirBody),
         "a block without an executable terminator should be rejected");

  body = representativeBody();
  body.blocks[0].terminator.target = body.blocks.size() + 1;
  const lang::CppMirControlFlowPlan badTarget =
      lang::buildCppMirControlFlowPlan(body);
  expect(hasIssue(lang::verifyCppMirControlFlowPlan(body, badTarget),
                  lang::CppMirControlFlowIssueKind::InvalidMirBody),
         "an edge outside the canonical block inventory should be rejected");
}

} // namespace

int main() {
  testDeterministicExactCoverage();
  testMalformedPlansFailClosed();
  testMalformedMirFailsClosed();

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "all C++ MIR control-flow plan tests passed\n";
  return 0;
}
