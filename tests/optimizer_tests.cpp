#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/optimization/effects.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

const lang::MirBody *findFunction(const lang::FrontendResult &frontend,
                                  const std::string &name) {
  for (const lang::HirFunctionInstance &instance :
       frontend.hir.functionInstances()) {
    if (instance.source == nullptr || instance.source->name().lexeme != name) {
      continue;
    }
    const lang::MirFunctionInstance *mir =
        frontend.mir.findFunctionInstance(instance.id);
    return mir == nullptr ? nullptr : &mir->body;
  }
  return nullptr;
}

void testMirIntegrityAndIdentityPipeline() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("optimizer-foundation.gti", R"(
class Counter {
  int32_t value;

public:
  Counter(int32_t initial) : value(initial) {}
  ~Counter() {}
  int32_t read() { return this.value; }
};

int32_t choose(bool condition) {
  if (condition) {
    return 1;
  }
  return 2;
}

int main() {
  Counter counter = Counter(1);
  return choose(counter.read() == 1);
}
)");
  expect(frontend.canGenerateCode(),
         "optimizer fixture should produce valid frontend IR");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirVerificationResult verified =
      lang::verifyMirProgram(frontend.mir);
  expect(verified.valid(),
         "the reusable verifier should accept frontend-produced MIR");

  const std::string before = lang::MirPrinter().print(frontend.mir);
  const std::string repeated = lang::MirPrinter().print(frontend.mir);
  expect(
      before == repeated && before.find("function @") != std::string::npos &&
          before.find("defined=bb") != std::string::npos &&
          before.find(" uses ") != std::string::npos,
      "MIR printing should be deterministic and expose definitions and uses");

  const lang::OptimizedProgram optimized = lang::OptimizationPipeline().run(
      lang::OptimizationRequest{.hir = frontend.hir,
                                .mir = frontend.mir,
                                .level = lang::OptimizationLevel::O2});
  expect(optimized.valid() && optimized.report.verificationEnabled &&
             optimized.report.passes.empty(),
         "the initial MIR pipeline should verify an identity snapshot without "
         "claiming transformations");
  expect(before == lang::MirPrinter().print(optimized.mir),
         "identity optimization should preserve the complete MIR snapshot");

  const lang::OptimizedProgram unchecked =
      lang::OptimizationPipeline().run(lang::OptimizationRequest{
          .hir = frontend.hir,
          .mir = frontend.mir,
          .level = lang::OptimizationLevel::O0,
          .options = lang::OptimizationOptions{.verifyMir = false}});
  expect(unchecked.valid() && !unchecked.report.verificationEnabled &&
             before == lang::MirPrinter().print(unchecked.mir),
         "disabling verification should not alter the identity snapshot");

  const lang::MirBody *function = findFunction(frontend, "choose");
  expect(function != nullptr, "optimizer fixture should expose choose MIR");
  if (function == nullptr) {
    return;
  }

  lang::MirBody staleUses = *function;
  staleUses.valueUses.clear();
  const lang::MirVerificationResult staleUseResult =
      lang::verifyMirBody(staleUses);
  expect(!staleUseResult.valid() && !staleUseResult.errors.empty() &&
             staleUseResult.errors.front().message.find("value-use index") !=
                 std::string::npos,
         "the verifier should diagnose a stale value-use index");
  expect(lang::rebuildMirValueUses(staleUses) &&
             lang::verifyMirBody(staleUses).valid(),
         "the shared use-index repair should restore valid MIR");

  lang::MirBody staleReachability = *function;
  for (lang::MirBlock &block : staleReachability.blocks) {
    block.reachable = !block.reachable;
  }
  const lang::MirVerificationResult staleReachabilityResult =
      lang::verifyMirBody(staleReachability);
  expect(!staleReachabilityResult.valid() &&
             !staleReachabilityResult.errors.empty() &&
             staleReachabilityResult.errors.front().message.find(
                 "reachability") != std::string::npos,
         "the verifier should diagnose stale reachability facts");
  lang::rebuildMirReachability(staleReachability);
  expect(lang::verifyMirBody(staleReachability).valid(),
         "the shared reachability repair should restore valid MIR");

  lang::MirBody malformed = *function;
  malformed.blocks.front().terminator.kind = lang::MirTerminatorKind::Goto;
  malformed.blocks.front().terminator.target = malformed.blocks.size() + 1;
  const lang::MirVerificationResult malformedResult =
      lang::verifyMirBody(malformed);
  expect(!malformedResult.valid() && !malformedResult.errors.empty() &&
             malformedResult.errors.front().block ==
                 malformed.blocks.front().id &&
             malformedResult.errors.front().message.find("goto target") !=
                 std::string::npos,
         "a malformed CFG rewrite should fail with a useful block diagnostic");
}

void testMirEffectClassification() {
  for (std::size_t index = 0; index < lang::mirInstructionKindCount; ++index) {
    const auto kind = static_cast<lang::MirInstructionKind>(index);
    expect(lang::name(kind) != "invalid",
           "every MIR instruction kind should have a stable classification");
    (void)lang::effects(kind);
  }
  for (std::size_t index = 0; index < lang::mirOperationCount; ++index) {
    const auto operation = static_cast<lang::MirOperation>(index);
    expect(lang::name(operation) != "invalid",
           "every MIR operation should have a stable classification");
    (void)lang::effects(operation);
  }
  for (std::size_t index = 0; index < lang::intrinsicKindCount; ++index) {
    const auto intrinsic = static_cast<lang::IntrinsicKind>(index);
    expect(lang::name(intrinsic) != "invalid",
           "every intrinsic should have a stable classification");
    (void)lang::effects(intrinsic);
  }

  const lang::MirEffectTraits literal = lang::effects(
      lang::MirInstruction{.kind = lang::MirInstructionKind::Compute,
                           .operation = lang::MirOperation::Literal});
  expect(literal.speculatable && literal.removableWhenUnused &&
             literal.reorderable && !literal.mayTrap,
         "literal computation should be classified as harmless");

  const lang::MirEffectTraits division = lang::effects(
      lang::MirInstruction{.kind = lang::MirInstructionKind::Compute,
                           .operation = lang::MirOperation::Divide});
  expect(
      division.mayTrap && !division.speculatable &&
          !division.removableWhenUnused,
      "division should remain non-removable until GTI edge semantics prove it");

  const lang::MirEffectTraits ordinaryCall = lang::effects(
      lang::MirInstruction{.kind = lang::MirInstructionKind::Call});
  expect(ordinaryCall.readsUnknownMemory && ordinaryCall.writesUnknownMemory &&
             ordinaryCall.invokesUserCode && ordinaryCall.mayTrap &&
             !ordinaryCall.removableWhenUnused,
         "ordinary calls should have conservative unknown effects");

  const lang::MirEffectTraits allocation = lang::effects(
      lang::MirInstruction{.kind = lang::MirInstructionKind::Call,
                           .intrinsic = lang::IntrinsicKind::AllocateStorage});
  expect(allocation.allocates && allocation.invokesRuntime &&
             allocation.writesUnknownMemory && allocation.mayTrap,
         "allocation intrinsics should expose allocation and runtime effects");

  const lang::MirEffectTraits drop = lang::effects(
      lang::MirInstruction{.kind = lang::MirInstructionKind::Drop});
  expect(drop.dropsValue && drop.invokesUserCode && drop.writesPlace &&
             !drop.reorderable,
         "drops should remain observable and ordered");
}

} // namespace

int main() {
  testMirIntegrityAndIdentityPipeline();
  testMirEffectClassification();

  if (failures != 0) {
    std::cerr << failures << " optimizer test(s) failed\n";
    return 1;
  }
  std::cout << "All optimizer tests passed\n";
  return 0;
}
