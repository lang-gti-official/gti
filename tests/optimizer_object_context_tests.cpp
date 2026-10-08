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
    expect(count(optimized, "after_address", "Gauge") == expected,
           "inert pointer arithmetic must preserve unrelated field facts "
           "without proving any pointee or address invariant");
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
  for (const auto name :
       {"optimizer_nested_write", "optimizer_reference_write",
        "optimizer_nested_raw", "optimizer_scalar_construction",
        "optimizer_scalar_construction_paths"}) {
    const auto path = sourcePath.parent_path() / (std::string(name) + ".gti");
    const std::ifstream file(path);
    const std::string text{std::istreambuf_iterator<char>(file.rdbuf()), {}};
    const auto library =
        sourcePath.parent_path().parent_path().parent_path() / "stdlib";
    const auto changed = lang::Frontend().analyze(
        path, text, {library / "prelude.gti", path},
        {{std::filesystem::weakly_canonical(path).string(), text}}, {library});
    expect(changed.canGenerateCode(),
           "nested and reference-field mutation controls must lower");
    if (!changed.canGenerateCode()) {
      for (const auto &diagnostic : changed.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
          lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = changed.hir, .mir = changed.mir, .level = level});
      expect(optimized.valid(), "nested mutation controls must replay");
      for (const auto &function : changed.hir.functionInstances()) {
        if (!function.source)
          continue;
        const auto functionName = function.source->name().lexeme;
        const bool construction = functionName == "construct_other" ||
                                  functionName == "nested_direct" ||
                                  functionName == "nested_reconstruct" ||
                                  functionName == "byte_direct" ||
                                  functionName == "array_direct";
        if (functionName != "read" && functionName != "reconstruct" &&
            !construction)
          continue;
        const auto *body = optimized.mir.findFunctionInstance(function.id);
        std::size_t proved = 0;
        if (body)
          for (const auto &block : body->body.blocks)
            for (const auto &instruction : block.instructions)
              proved += std::count_if(
                  instruction.safetyDischarges.begin(),
                  instruction.safetyDischarges.end(), [](const auto &proof) {
                    return proof.family ==
                           lang::MirSafetyCheckFamily::ArithmeticOverflow;
                  });
        expect(
            proved ==
                (construction && level != lang::OptimizationLevel::O0 ? 1 : 0),
            "nested, returned and borrowed-field aliases cannot retain "
            "stale constructor envelopes after mutation; scalar vacant "
            "construction preserves unrelated active fields");
      }
    }
  }
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
  // Each successive exact callee summary makes one more call reachable. This
  // deliberately consumes the per-body analysis budget, after the first
  // contextual arithmetic proof has already been observed.
  std::string budget;
  for (std::size_t index = 0; index < 40; ++index)
    budget += "int child" + std::to_string(index) + "() { return 0; }\n";
  budget += "int budget(int input) { int first = input + 1;\n";
  for (std::size_t index = 0; index < 40; ++index)
    budget += "[[discard]] child" + std::to_string(index) + "();\n";
  budget += "return first; }\n"
            "int independent(int input) { return input + 1; }\n"
            "int main() { int first = budget(7); int second = independent(7); "
            "return first == 8 && second == 8 ? 0 : 1; }\n";
  const auto bounded =
      lang::Frontend().analyze("object-summary-budget.gti", budget);
  expect(bounded.canGenerateCode(), "the work-budget control must lower");
  if (bounded.canGenerateCode()) {
    lang::HirFunctionInstanceId budgetOwner = 0;
    lang::HirFunctionInstanceId independentOwner = 0;
    for (const auto &function : bounded.hir.functionInstances()) {
      if (!function.source)
        continue;
      if (function.source->name().lexeme == "budget")
        budgetOwner = function.id;
      if (function.source->name().lexeme == "independent")
        independentOwner = function.id;
    }
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = budgetOwner};
    const auto analysis = lang::analyzeMirClosedCallSafetyChecks(bounded.mir);
    const auto *context = analysis.find(address);
    expect(
        analysis.converged && analysis.iterations <= 32 && context &&
            !context->closed && context->safety.proofs.empty(),
        "budget rejection withdraws earlier contextual proofs and converges");
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
          lang::OptimizationLevel::O3}) {
      const auto optimized = lang::OptimizationPipeline().run(
          {.hir = bounded.hir, .mir = bounded.mir, .level = level});
      expect(optimized.valid(), "budget rejection must independently replay");
      const auto countProofs = [&](lang::HirFunctionInstanceId owner) {
        std::size_t count = 0;
        const auto *body = optimized.mir.findFunctionInstance(owner);
        if (body)
          for (const auto &block : body->body.blocks)
            for (const auto &instruction : block.instructions)
              count += instruction.safetyDischarges.size();
        return count;
      };
      expect(countProofs(budgetOwner) == 0 &&
                 countProofs(independentOwner) ==
                     (level == lang::OptimizationLevel::O0 ? 0 : 1),
             "rejecting one body's work budget cannot keep its stale proof or "
             "hide a subsequent independent exact client");
    }
    bool rejected = false;
    if (const auto *body = lang::findMirBody(bounded.mir, address))
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions)
          for (std::size_t origin = 0;
               origin < instruction.definedFailure.localOrigins.size();
               ++origin)
            for (const auto &outcome :
                 instruction.definedFailure.localOrigins[origin].outcomes) {
              if (outcome.code != lang::DefinedFailureCode::IntegerOverflow)
                continue;
              auto malformed = bounded.mir;
              lang::MirEditor editor(malformed, "forged-rejected-budget-proof");
              editor.queueSafetyDischarge(
                  {.body = address, .instruction = instruction.id},
                  {.family = lang::MirSafetyCheckFamily::ArithmeticOverflow,
                   .origin = origin,
                   .outcome = outcome});
              const auto result = editor.apply();
              expect(!result.valid() && !result.changed,
                     "the canonical editor rejects a withdrawn budget proof");
              rejected = true;
            }
    expect(rejected, "budget rejection exercises canonical proof rejection");
  }
  return failures == 0 ? 0 : 1;
}
