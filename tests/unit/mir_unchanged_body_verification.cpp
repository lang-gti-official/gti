// Intermediate MIR verification rejects the same body edits and program-level
// contract changes as a full verification, with the same diagnostics.
#include "../../src/compiler/mir_verification_support.h"
#include "gti/frontend.h"
#include "gti/optimizer.h"
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

GTI_UNIT_TEST(mir_optimizer_owns_verified_input) {
  using namespace lang;
  const auto frontend =
      Frontend().analyze("source.gti", "int main() { return 0; }");
  if (!frontend.canGenerateCode())
    return 1;
  auto optimize = [&](MirProgram input) {
    return OptimizationPipeline().run(
        OptimizationRequest{.hir = frontend.hir,
                            .mir = std::move(input),
                            .level = OptimizationLevel::O3});
  };
  MirProgram input = frontend.mir;
  const auto *functions = input.functionInstances().data();
  const auto accepted = optimize(std::move(input));
  if (!accepted.valid() || accepted.sourceMir != frontend.mir ||
      accepted.sourceMir.functionInstances().data() != functions ||
      accepted.mir.functionInstances().data() == functions) {
    std::cerr
        << "optimizer must keep the submitted MIR as its immutable source\n";
    return 1;
  }

  MirProgram forged = frontend.mir;
  const auto address = MirBodyAddress{MirBodyKind::Function,
                                      forged.functionInstances().front().id};
  findMirBody(forged, address)->entry = 0;

  // Track ownership of a different, previously rejected program. The input
  // cache miss below must release it even when optimization exits on error.
  std::weak_ptr<const GenericValueExpression> obsolete;
  {
    auto previous = forged;
    auto expression = std::make_shared<GenericValueExpression>();
    obsolete = expression;
    auto &type = findMirBody(previous, address)->returnType;
    type.valueArguments.push_back({.expression = std::move(expression)});
    if (verifyMirProgram(previous).valid())
      return 1;
  }
  const auto cold = optimize(forged);
  if (!obsolete.expired()) {
    std::cerr << "optimizer retained an obsolete verifier snapshot\n";
    return 1;
  }
  const auto full = verifyMirProgram(forged);
  const auto cached = optimize(forged);
  const auto sameError = [](const auto &a, const auto &b) {
    return a.bodyKind == b.bodyKind && a.owner == b.owner &&
           a.block == b.block && a.instruction == b.instruction &&
           a.message == b.message && a.borrow == b.borrow;
  };
  if (full.valid() ||
      !std::ranges::equal(full.errors, cold.report.inputVerification.errors,
                          sameError) ||
      !std::ranges::equal(full.errors, cached.report.inputVerification.errors,
                          sameError) ||
      cold.mir != forged || cached.sourceMir != forged)
    return 1;
  return 0;
}
