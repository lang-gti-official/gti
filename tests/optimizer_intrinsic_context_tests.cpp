#include "gti/frontend.h"
#include "gti/optimization/pass.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
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

int changed(mut BoundsClient& object, uint64_t index, uint64_t length) {
  object.value = 2147483647;
  gti_internal::index_bounds_check(index, length);
  return object.value + 1;
}

int main() {
  mut BoundsClient object{7};
  // Seed the field explicitly: parameterized member-initializer summaries
  // are a separate prerequisite, not the bounds detector's responsibility.
  object.value = 7;
  int value = checked(object, uint64_t(0), uint64_t(2));
  if (value != 8) return 1;
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
  }
  return failures == 0 ? 0 : 1;
}
