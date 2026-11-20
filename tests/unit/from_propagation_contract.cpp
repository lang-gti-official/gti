// HIR retains the exact conversion selected for a propagation failure exit.
#include "gti/frontend.h"
#include "gti/hir.h"
#include "gti/mir_verifier.h"
#include "unit_test.h"
#include <fstream>
#include <iterator>

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

GTI_UNIT_TEST(from_empty_result_needs_normal_return) {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
  const auto entry = root / "tests/ui/expected/from/reference_success.gti";
  std::ifstream input(entry);
  const std::string source((std::istreambuf_iterator<char>(input)), {});
  auto frontend = lang::Frontend().analyze(entry, source,
                                           {root / "stdlib/prelude.gti", entry},
                                           {}, {root / "stdlib"});
  if (!frontend.canGenerateCode())
    return 1;
  lang::HirFunctionInstanceId adapter = 0;
  for (const auto &function : frontend.hir.functionInstances())
    if (function.source && function.source->name().lexeme == "from" &&
        function.returnType.kind == lang::SemanticType::Class &&
        function.returnBorrowOrigin == lang::BorrowOriginKind::Global)
      adapter = function.id;
  if (!adapter)
    return 2;
  auto forged = frontend.mir;
  bool changed = false;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      forged.functionInstances());
  for (auto &function : functions)
    if (function.id == adapter)
      for (auto &block : function.body.blocks)
        if (block.terminator.kind == lang::MirTerminatorKind::Return) {
          block.terminator = {.kind = lang::MirTerminatorKind::Unreachable};
          changed = true;
        }
  if (!changed)
    return 3;
  const auto result = lang::verifyMirProgram(forged);
  for (const auto &error : result.errors)
    if (error.message.find(
            "owner-dependent return is missing its escaping loan") !=
        std::string::npos)
      return 0;
  return 4;
}
