// HIR retains the exact conversion selected for a propagation failure exit.
#include "gti/frontend.h"
#include "gti/hir.h"
#include "unit_test.h"

GTI_UNIT_TEST(from_propagation_contract) {
  const std::string source = R"(
struct Rejected { int code; Rejected(int n) : code(n) {} };
struct Checked { int code; Checked(int n) : code(n) {} };
enum Outcome { value(int n), failed(int n) };
for concept std::tryable<Checked> {
  using value_type = int;
  using residual_type = Rejected;
  bool has_value(Checked& value) { return value.code == 0; }
  int take_value(Checked value) { return 42; }
  Rejected take_residual(Checked value) { return Rejected(value.code); }
};
for concept std::from<Outcome, Rejected> {
  Outcome from(Rejected error) { return Outcome::failed(error.code); }
};
Outcome decoy(Rejected error) { return Outcome::value(error.code); }
Outcome calculate() { return Outcome::value(?Checked(7)); }
int main() {
  auto result = calculate();
  auto other = decoy(Rejected(1));
  return 0;
}
)";
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
  const auto entry =
      std::filesystem::temp_directory_path() / "gti-from-contract.gti";
  auto frontend = lang::Frontend().analyze(
      entry, source, {root / "stdlib/prelude.gti", entry},
      {{std::filesystem::weakly_canonical(entry).string(), source}},
      {root / "stdlib"});
  if (!frontend.canGenerateCode() ||
      !lang::verifyHirProgramPlans(frontend.semantics, frontend.hir).valid())
    return 1;
  lang::HirFunctionInstanceId decoy = 0;
  for (const auto &function : frontend.hir.functionInstances())
    if (function.source && function.source->name().lexeme == "decoy")
      decoy = function.id;
  if (!decoy)
    return 2;
  auto forged = frontend.hir;
  auto &functions = const_cast<std::vector<lang::HirFunctionInstance> &>(
      forged.functionInstances());
  bool changed = false;
  for (auto &function : functions) {
    if (!function.source || function.source->name().lexeme != "calculate")
      continue;
    for (const auto &value : function.body.values) {
      if (!value.propagation || value.propagation->residualReturn == 0)
        continue;
      const auto target = value.propagation->residualReturn;
      for (auto &step : function.body.values)
        if (step.id == target && step.functionTarget) {
          // Same parameter and return types, but the wrong selected function.
          step.functionTarget = decoy;
          changed = true;
        }
    }
  }
  return changed && !lang::verifyHirProgramPlans(frontend.semantics, forged)
                         .valid()
             ? 0
             : 3;
}
