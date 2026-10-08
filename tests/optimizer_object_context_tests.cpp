#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/optimization/pass.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

int main() {
  const auto sourcePath = std::filesystem::path(__FILE__).parent_path() /
                          "fixtures/optimizer_object_context.gti";
  const std::ifstream sourceFile(sourcePath);
  const std::string source{std::istreambuf_iterator<char>(sourceFile.rdbuf()),
                           {}};
  const auto frontend = lang::Frontend().analyze(sourcePath, source);
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  int failures = 0;
  const auto expect = [&](bool valid, std::string_view message) {
    if (!valid) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  };
  const auto count = [&](const lang::OptimizedProgram &optimized,
                         std::string_view name, std::string_view owner) {
    std::size_t result = 0;
    for (const auto &instance : frontend.hir.functionInstances()) {
      if (!instance.source || instance.source->name().lexeme != name)
        continue;
      const lang::HirClassInstance *classInstance = nullptr;
      if (instance.owner)
        for (const auto &candidate : frontend.hir.classInstances())
          if (candidate.id == *instance.owner)
            classInstance = &candidate;
      if (!owner.empty() &&
          (!classInstance || classInstance->source->name().lexeme != owner))
        continue;
      const auto *function = optimized.mir.findFunctionInstance(instance.id);
      if (function)
        for (const auto &block : function->body.blocks)
          for (const auto &instruction : block.instructions)
            result += std::count_if(
                instruction.safetyDischarges.begin(),
                instruction.safetyDischarges.end(), [](const auto &proof) {
                  return proof.family ==
                         lang::MirSafetyCheckFamily::ArithmeticOverflow;
                });
    }
    return result;
  };
  const auto contextual = lang::analyzeMirClosedCallSafetyChecks(frontend.mir);
  expect(contextual.converged, "ordinary object contexts must converge");
  for (const auto &function : frontend.hir.functionInstances()) {
    if (!function.source)
      continue;
    const auto name = function.source->name().lexeme;
    if (name != "changed" && name != "raw_changed" && name != "unknown")
      continue;
    const auto *context = contextual.find(
        {.kind = lang::MirBodyKind::Function, .owner = function.id});
    expect(context && context->reached == (name != "unknown"),
           "mutation negatives are reached; only the explicit unknown control "
           "is unreached");
  }
  for (const auto level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
        lang::OptimizationLevel::O3}) {
    const auto optimized = lang::OptimizationPipeline().run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
    expect(optimized.valid(), "ordinary object field proofs must replay");
    for (const auto &record : optimized.report.transformations)
      expect(std::find(record.relatedPlaces.begin(), record.relatedPlaces.end(),
                       0) == record.relatedPlaces.end(),
             "implicit receiver roots must never be published as MIR place "
             "identities");
    const std::size_t expected = level == lang::OptimizationLevel::O0 ? 0 : 1;
    expect(count(optimized, "read", "Gauge") == expected,
           "parameterized constructor and owned result fields reach an "
           "implicit receiver");
    expect(count(optimized, "bump", "Gauge") == 2 * expected,
           "a field mutation retains an independent declaration-default field");
    expect(count(optimized, "read", "Leaf") == expected,
           "a nested field constructor publishes facts relative to its exact "
           "subobject");
    expect(
        count(optimized, "before", "Aliased") == expected,
        "the alias-write negative starts from an independently proved field");
    expect(count(optimized, "read", "Hazard") == 0 &&
               count(optimized, "changed", "Hazard") == 0 &&
               count(optimized, "raw_changed", "Hazard") == 0 &&
               count(optimized, "changed", "Aliased") == 0 &&
               count(optimized, "read", "RawConstructed") == 0 &&
               count(optimized, "unknown", "") == 0,
           "overflowing, raw-mutated and unreached object fields stay checked");
    if (level != lang::OptimizationLevel::O0) {
      auto forged = optimized.report.transformations;
      const auto proof =
          std::find_if(forged.begin(), forged.end(), [](const auto &record) {
            return record.kind == lang::MirTransformKind::DischargeSafetyCheck;
          });
      expect(proof != forged.end(),
             "object facts require durable proof evidence");
      if (proof != forged.end()) {
        proof->relatedPlaces.push_back(
            std::numeric_limits<lang::MirPlaceId>::max());
        expect(!lang::verifyMirOptimizationCoherence(frontend.mir,
                                                     optimized.mir, forged)
                    .valid(),
               "forged object-field evidence must not replay");
      }
    }
  }
  bool testedForgery = false;
  for (const auto &function : frontend.hir.functionInstances()) {
    if (!function.source || function.source->name().lexeme != "raw_changed")
      continue;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function.id};
    const auto *body = lang::findMirBody(frontend.mir, address);
    if (!body)
      continue;
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions)
        for (std::size_t origin = 0;
             origin < instruction.definedFailure.localOrigins.size(); ++origin)
          for (const auto &outcome :
               instruction.definedFailure.localOrigins[origin].outcomes) {
            if (outcome.code != lang::DefinedFailureCode::IntegerOverflow)
              continue;
            auto malformed = frontend.mir;
            lang::MirEditor editor(malformed, "forged-post-raw-object-field");
            editor.queueSafetyDischarge(
                {.body = address, .instruction = instruction.id},
                {.family = lang::MirSafetyCheckFamily::ArithmeticOverflow,
                 .origin = origin,
                 .outcome = outcome});
            const auto result = editor.apply();
            expect(!result.valid() && !result.changed,
                   "the canonical editor rejects stale constructor facts after "
                   "raw writes");
            testedForgery = true;
          }
  }
  expect(testedForgery,
         "the raw-field negative must exercise canonical rejection");
  constexpr std::size_t chainLength = 128;
  std::string chain;
  for (std::size_t index = chainLength; index-- > 0;) {
    chain += "int step" + std::to_string(index) + "(int value) { return ";
    chain += index + 1 == chainLength
                 ? "value"
                 : "step" + std::to_string(index + 1) + "(value)";
    chain += " + 1; }\n";
  }
  chain += "int main() { return step0(7) == 135 ? 0 : 1; }\n";
  const auto deep = lang::Frontend().analyze("object-summary-chain.gti", chain);
  expect(deep.canGenerateCode(), "the long ordinary source chain must lower");
  if (deep.canGenerateCode()) {
    const auto analysis = lang::analyzeMirClosedCallSafetyChecks(deep.mir);
    expect(analysis.converged && analysis.iterations <= 32,
           "fixed facts traverse 128 bodies within the per-body work budget");
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
          lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = deep.hir, .mir = deep.mir, .level = level});
      expect(optimized.valid(),
             "long-chain contextual proofs independently replay");
      std::size_t proved = 0;
      for (const auto &function : optimized.mir.functionInstances())
        for (const auto &block : function.body.blocks)
          for (const auto &instruction : block.instructions)
            proved += std::count_if(
                instruction.safetyDischarges.begin(),
                instruction.safetyDischarges.end(), [](const auto &proof) {
                  return proof.family ==
                         lang::MirSafetyCheckFamily::ArithmeticOverflow;
                });
      expect(proved == (level == lang::OptimizationLevel::O0 ? 0 : chainLength),
             "every exact addition in the long chain is proved; O0 stays "
             "unchanged");
    }
  }
  return failures == 0 ? 0 : 1;
}
