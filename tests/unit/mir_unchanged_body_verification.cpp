// Intermediate MIR verification rejects the same body edits and program-level
// contract changes as a full verification, with the same diagnostics.
#include "../../src/compiler/mir_verification_support.h"
#include "gti/frontend.h"
#include "unit_test.h"

#include <algorithm>
#include <iostream>

GTI_UNIT_TEST(mir_unchanged_body_verification) {
  using namespace lang;
  const FrontendResult frontend = Frontend().analyze(
      "reuse.gti",
      "int f(int n) { return n + 1; } int main() { return f(3); }");
  const MirProgram &input = frontend.mir;
  if (!frontend.canGenerateCode() || !verifyMirProgram(input).valid() ||
      input.functionInstances().size() != 2 ||
      !mir_detail::verifyMirProgramWithVerifiedBodies(input, &input).valid())
    return 1;

  const auto id = input.functionInstances().front().id;
  const MirBodyAddress address{MirBodyKind::Function, id};
  const auto rejectedLikeFull = [&](const MirProgram &candidate) {
    const auto full = verifyMirProgram(candidate);
    const auto edited =
        mir_detail::verifyMirProgramWithVerifiedBodies(candidate, &input);
    return !full.valid() &&
           std::ranges::equal(
               full.errors, edited.errors, [](const auto &a, const auto &b) {
                 return a.bodyKind == b.bodyKind && a.owner == b.owner &&
                        a.block == b.block && a.instruction == b.instruction &&
                        a.message == b.message && a.borrow == b.borrow;
               });
  };

  MirProgram changed = input;
  findMirBody(changed, address)->entry = 0;
  if (!rejectedLikeFull(changed)) {
    std::cerr << "missed a changed entry block\n";
    return 1;
  }

  changed = input;
  auto &debug = findMirBody(changed, address)->sourceDebug;
  debug.collection = SourceDebugCollection::CollectedLexicalV1;
  debug.tables.reset();
  if (!rejectedLikeFull(changed)) {
    std::cerr << "missed a change confined to debug tables\n";
    return 1;
  }

  changed = input;
  auto *callee =
      const_cast<MirFunctionInstance *>(changed.findFunctionInstance(id));
  callee->returnType = SemanticType::Void;
  if (!rejectedLikeFull(changed)) {
    std::cerr << "missed a program contract change with all bodies identical\n";
    return 1;
  }
  return 0;
}
