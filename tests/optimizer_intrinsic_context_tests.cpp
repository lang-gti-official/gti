#include "gti/frontend.h"
#include "gti/optimization/pass.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

int main() {
  const auto standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto entry = standardLibrary / "optimizer-bounds-context.gti";
  const std::string source = R"gti(
class BoundsClient {
public:
  mut int value;
  BoundsClient(int value) : value(value) {}
};

int checked(BoundsClient& object, uint64_t index, uint64_t length) {
  gti_internal::index_bounds_check(index, length);
  return object.value + 1;
}

int unknown(BoundsClient& object, uint64_t index, uint64_t length) {
  gti_internal::index_bounds_check(index, length);
  return object.value + 1;
}

int checked_condition(BoundsClient& object, bool condition) {
  gti_internal::require_allocation(condition);
  return object.value + 1;
}

int changed_condition(mut BoundsClient& object, bool condition) {
  object.value = 2147483647;
  gti_internal::require_allocation(condition);
  return object.value + 1;
}

int changed(mut BoundsClient& object, uint64_t index, uint64_t length) {
  object.value = 2147483647;
  gti_internal::index_bounds_check(index, length);
  return object.value + 1;
}

int main() {
  mut BoundsClient object{7};
  int value = checked(object, uint64_t(0), uint64_t(2));
  if (value != 8) return 1;
  if (checked_condition(object, true) != 8) return 2;
  if (changed_condition(object, true) != 8) return 3;
  return changed(object, uint64_t(0), uint64_t(2));
}
)gti";
  // Only the detector call is privileged. The record and its ordinary
  // constructor/field facts are independent of every library owner spelling.
  const auto frontend = lang::Frontend().analyze(
      entry, source, {standardLibrary / "prelude.gti", entry},
      {{std::filesystem::weakly_canonical(entry).string(), source}},
      {standardLibrary});
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
  const auto function = [&](std::string_view name) {
    for (const auto &instance : frontend.hir.functionInstances())
      if (instance.source && instance.source->name().lexeme == name)
        return instance.id;
    return lang::HirFunctionInstanceId{};
  };
  const auto contextual = lang::analyzeMirClosedCallSafetyChecks(frontend.mir);
  const auto *checked = contextual.find(
      {.kind = lang::MirBodyKind::Function, .owner = function("checked")});
  expect(
      contextual.converged && checked && checked->reached && checked->closed,
      "a scalar bounds detector must not make an ordinary source body opaque");
  const lang::OptimizationPipeline pipeline;
  for (const auto level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
        lang::OptimizationLevel::O3}) {
    const auto optimized = pipeline.run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
    expect(optimized.valid(), "bounds/field facts must independently replay");
    const auto count = [&](std::string_view name,
                           lang::MirSafetyCheckFamily family) {
      std::size_t result = 0;
      const auto *body = optimized.mir.findFunctionInstance(function(name));
      if (body)
        for (const auto &block : body->body.blocks)
          for (const auto &instruction : block.instructions)
            result += std::count_if(
                instruction.safetyDischarges.begin(),
                instruction.safetyDischarges.end(),
                [family](const auto &proof) { return proof.family == family; });
      return result;
    };
    const std::size_t expected = level == lang::OptimizationLevel::O0 ? 0 : 1;
    expect(
        count("checked", lang::MirSafetyCheckFamily::Bounds) == expected &&
            count("checked", lang::MirSafetyCheckFamily::ArithmeticOverflow) ==
                expected,
        "proving a bounds check must retain the unrelated receiver field fact: "
        "bounds=" +
            std::to_string(
                count("checked", lang::MirSafetyCheckFamily::Bounds)) +
            ", arithmetic=" +
            std::to_string(count(
                "checked", lang::MirSafetyCheckFamily::ArithmeticOverflow)));
    expect(count("unknown", lang::MirSafetyCheckFamily::Bounds) == 0 &&
               count("unknown",
                     lang::MirSafetyCheckFamily::ArithmeticOverflow) == 0 &&
               count("changed",
                     lang::MirSafetyCheckFamily::ArithmeticOverflow) == 0,
           "unknown bounds/fields and overflowing field writes remain checked");
    expect(count("checked_condition",
                 lang::MirSafetyCheckFamily::ArithmeticOverflow) == expected &&
               count("changed_condition",
                     lang::MirSafetyCheckFamily::ArithmeticOverflow) == 0,
           "a pure boolean detector preserves unrelated facts without hiding "
           "an overflowing mutation");
  }
  const auto guardPath = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures/optimizer_boolean_guards.gti";
  const std::ifstream guardFile(guardPath);
  const std::string guardSource{
      std::istreambuf_iterator<char>(guardFile.rdbuf()), {}};
  const auto guards = lang::Frontend().analyze(
      guardPath, guardSource, {standardLibrary / "prelude.gti", guardPath},
      {{std::filesystem::weakly_canonical(guardPath).string(), guardSource}},
      {standardLibrary});
  expect(guards.canGenerateCode(),
         "source predicate guard controls must lower");
  if (!guards.canGenerateCode())
    for (const auto &diagnostic : guards.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  if (guards.canGenerateCode())
    for (const auto level :
         {lang::OptimizationLevel::O0, lang::OptimizationLevel::O2,
          lang::OptimizationLevel::O3}) {
      const auto optimized =
          pipeline.run({.hir = guards.hir, .mir = guards.mir, .level = level});
      expect(optimized.valid(), "source predicate guard proofs must replay");
      for (const auto &instance : guards.hir.functionInstances()) {
        if (!instance.source)
          continue;
        const auto name = instance.source->name().lexeme;
        if (name != "bounded" && name != "inverse" && name != "changed")
          continue;
        const auto *body = optimized.mir.findFunctionInstance(instance.id);
        std::size_t count = 0;
        if (body)
          for (const auto &block : body->body.blocks)
            for (const auto &instruction : block.instructions)
              for (const auto &proof : instruction.safetyDischarges)
                count += proof.family ==
                         lang::MirSafetyCheckFamily::ArithmeticOverflow;
        expect(count ==
                   (name != "changed" && level != lang::OptimizationLevel::O0
                        ? 1
                        : 0),
               "successful boolean guards refine staged predicates; a later "
               "write must not retain the earlier place bound: " +
                   std::string(name) + " has " + std::to_string(count));
      }
    }
  return failures == 0 ? 0 : 1;
}
