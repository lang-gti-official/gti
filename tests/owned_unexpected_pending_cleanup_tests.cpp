#include "cpp_mir_body_emitter_test_fixture.h"

int main() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/owned_unexpected_pending_cleanup.gti";
  std::ifstream input(path);
  expect(input.good(), "pending Unexpected fixture exists");
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "Unexpected stays locally owned through fallible return cleanup");
  if (!frontend.canGenerateCode())
    return 1;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  std::size_t pendingReturns = 0;
  bool expectedRecipe = false;
  bool unexpectedRecipe = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &selected : function.body.dropObligations) {
      const bool expected =
          selected.dropType.type.kind == lang::SemanticType::Expected;
      const bool unexpected =
          selected.dropType.type.kind == lang::SemanticType::Unexpected;
      if ((!expected && !unexpected) || (expected && expectedRecipe) ||
          (unexpected && unexpectedRecipe))
        continue;
      expectedRecipe |= expected;
      unexpectedRecipe |= unexpected;
      expect(selected.dropType.aggregate.size() == (expected ? 2U : 1U) &&
                 selected.dropType.aggregate.back().requiresActiveCleanup &&
                 selected.dropType.aggregate.back().destructor,
             "alternative recipe retains its exact active error destructor");
      if (selected.dropType.aggregate.empty())
        continue;
      for (int mutation = 0; mutation != 4; ++mutation) {
        auto malformed = frontend.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        for (auto &drop : changed->body.dropObligations) {
          if (drop.dropType.type != selected.dropType.type)
            continue;
          if (mutation == 0)
            drop.dropType.aggregate.clear();
          if (mutation == 1)
            drop.dropType.aggregate.back().requiresActiveCleanup = false;
          if (mutation == 2)
            drop.dropType.aggregate.back().destructor.reset();
          if (mutation == 3)
            drop.dropType.aggregate.back().type = lang::SemanticType::Int32;
        }
        expect(!lang::verifyMirProgram(malformed).valid(),
               "independent MIR recipe derivation rejects missing or forged "
               "alternative cleanup");
        expect(!lang::CppMirBodyEmitter(malformed, rows)
                    .supportsFailureBodyText(
                        {lang::MirBodyKind::Function, function.id}),
               "backend cannot erase or change active alternative cleanup");
      }
    }
    for (const auto &pending : function.body.dropObligations) {
      if (pending.kind != lang::MirDropObligationKind::ReturnResult ||
          pending.dropType.type.kind != lang::SemanticType::Unexpected)
        continue;
      ++pendingReturns;
      const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                         function.id};
      const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
      expect(emitter.supportsFailureBodyText(address),
             "pending Unexpected supports its exact cleanup-only continuation");
      if (emitter.supportsFailureBodyText(address)) {
        const auto text = emitter.emitFailureBodyText(address, "test", 0);
        expect(
            text.emitted() &&
                text.text.find("// GTI MIR unpublished Unexpected cleanup") !=
                    std::string::npos &&
                text.text.find("__gti_mir_out_result->value()") ==
                    std::string::npos,
            "rollback destroys local Unexpected rather than unpublished "
            "Expected output");
      }
      expect(pending.dropType.type.arguments.size() == 1 &&
                 function.returnType.kind == lang::SemanticType::Expected &&
                 pending.dropType.type.arguments.front() ==
                     function.returnType.arguments.back(),
             "pending Unexpected carries the exact returned error type");
      for (int mutation = 0; mutation != 6; ++mutation) {
        auto malformed = function.body;
        if (mutation == 0)
          malformed.returnType.arguments.back() = lang::SemanticType::Int32;
        for (auto &drop : malformed.dropObligations)
          if (drop.id == pending.id && mutation == 1)
            drop.initiallyActive = true;
        for (auto &block : malformed.blocks)
          for (auto &instruction : block.instructions)
            for (auto &event : instruction.lifecycle) {
              if (event.kind == lang::MirLifecycleEventKind::Reparent &&
                  event.target == pending.id) {
                if (mutation == 2)
                  event.conditional = true;
                if (mutation == 3) {
                  event.kind = lang::MirLifecycleEventKind::TransferOut;
                  event.target = 0;
                }
              }
              if (event.kind == lang::MirLifecycleEventKind::TransferOut &&
                  event.source == pending.id) {
                if (mutation == 4)
                  event.failureCleanup = true;
                if (mutation == 5)
                  event.conditional = true;
              }
            }
        const auto result = lang::verifyMirBody(malformed, function.id);
        expect(
            !result.valid(),
            "pending Unexpected rejects malformed lifetime or error identity");
        auto malformedProgram = frontend.mir;
        const_cast<lang::MirFunctionInstance *>(
            malformedProgram.findFunctionInstance(function.id))
            ->body = malformed;
        expect(!lang::CppMirBodyEmitter(malformedProgram, rows)
                    .supportsFailureBodyText(address),
               "emission rejects altered pending Unexpected ownership");
        if (mutation == 3)
          expect(std::any_of(result.errors.begin(), result.errors.end(),
                             [](const auto &error) {
                               return error.message.find(
                                          "return ownership transfers before "
                                          "fallible cleanup") !=
                                      std::string::npos;
                             }),
                 "Unexpected cannot escape ownership before cleanup succeeds");
      }
    }
  }
  expect(pendingReturns == 1,
         "fixture retains one pending Unexpected rollback lifetime");
  expect(
      expectedRecipe && unexpectedRecipe,
      "fixture covers both completed Expected and pending Unexpected recipes");
  return failures ? 1 : 0;
}
