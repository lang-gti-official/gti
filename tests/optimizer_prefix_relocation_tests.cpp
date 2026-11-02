#include "gti/frontend.h"
#include "gti/optimization/safety.h"
#include "private_stdlib_fixture.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path();
  std::ifstream file(root / "fixtures/optimizer_prefix_relocation.gti");
  const std::string source{std::istreambuf_iterator<char>(file), {}};
  auto frontend = gti_test::analyzeTrustedPrivateMemoryFixture(
      "optimizer-prefix-relocation.gti", source);
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  const auto customMove = lang::Frontend().analyze(
      "prefix-custom-move-negative.gti", std::string(R"(
class Changed { int value; public:
  Changed(int value) : value(value) {}
  Changed(Changed&& other) : value(7) {}
};
int main() { return 0; }
)"));
  bool rejectedCustomMove = false;
  for (const auto &diagnostic : customMove.diagnostics)
    rejectedCustomMove |= diagnostic.code == "GTI-S2020";
  if (customMove.canGenerateCode() || !rejectedCustomMove) {
    std::cerr
        << "custom move support changed; review relocation proof exclusion\n";
    return 1;
  }
  const auto analysis = lang::analyzeMirClosedCallSafetyChecks(frontend.mir);
  if (!analysis.converged) {
    std::cerr << "relocation contextual analysis did not converge\n";
    return 1;
  }
  const auto discharges = [&](const std::string &name) {
    std::size_t found = 0;
    for (const auto &function : frontend.hir.functionInstances()) {
      if (!function.source || function.source->name().lexeme != name)
        continue;
      const auto *body = analysis.find(
          {.kind = lang::MirBodyKind::Function, .owner = function.id});
      if (!body)
        continue;
      const auto *mir = frontend.mir.findFunctionInstance(function.id);
      if (!mir)
        continue;
      for (const auto &proof : body->safety.proofs) {
        if (proof.discharge.family !=
            lang::MirSafetyCheckFamily::ArithmeticOverflow)
          continue;
        for (const auto &block : mir->body.blocks)
          for (const auto &instruction : block.instructions)
            if (instruction.id == proof.instruction &&
                instruction.operation == lang::MirOperation::Multiply)
              ++found;
      }
    }
    return found;
  };
  const auto positive = discharges("transferred_borrow_positive");
  const auto view = discharges("view_borrow_positive");
  const auto replaced = discharges("replaced_base");
  const auto scalar =
      discharges("transferred_positive") + discharges("view_positive");
  const auto changed = discharges("changed_destination");
  const auto partial = discharges("partial_transfer");
  const auto failed = discharges("failing_transfer");
  if (positive != 1 || view != 1 || changed != 0 || failed != 0 ||
      replaced != 0 || partial != 0 || scalar != 0) {
    std::cerr << "prefix relocation proof mismatch: successful=" << positive
              << " raw-view=" << view << " replaced=" << replaced
              << " overwritten=" << changed << " scalar-getters=" << scalar
              << " partial=" << partial << " failed=" << failed << '\n';
    for (const auto &function : frontend.hir.functionInstances())
      if (function.source)
        std::cerr << "PREFIX-MAP body=" << function.id << " name="
                  << function.source->name().lexeme << '\n';
    return 1;
  }
  return 0;
}
