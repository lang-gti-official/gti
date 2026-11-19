// Constant temporary borrows still require exact publication, lifetime and loan
// proofs.
#include "gti/frontend.h"
#include "gti/mir_verifier.h"
#include "unit_test.h"

#include <iostream>
#include <vector>

GTI_UNIT_TEST(implicit_argument_constant_forgery) {
  const auto frontend = lang::Frontend().analyze("constant-borrow.gti", R"(
struct Checked { int value; implicit consteval Checked(int n) : value(n) {} };
int read(Checked& value) { return value.value; }
int main() { return read(21) - 21; }
)");
  if (!frontend.canGenerateCode() ||
      !lang::verifyMirProgram(frontend.mir).valid()) {
    std::cerr << "constant temporary fixture did not produce valid MIR\n";
    return 1;
  }
  for (int mutation = 0; mutation != 3; ++mutation) {
    auto forged = frontend.mir;
    bool changed = false;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        forged.functionInstances());
    for (auto &function : functions) {
      for (auto &block :
           const_cast<std::vector<lang::MirBlock> &>(function.body.blocks)) {
        for (auto &instruction :
             const_cast<std::vector<lang::MirInstruction> &>(
                 block.instructions)) {
          if (changed)
            continue;
          if (mutation < 2 && instruction.programConstantSubstitution &&
              instruction.info.type.kind == lang::SemanticType::Class) {
            if (mutation == 0)
              instruction.programConstantSubstitution = false;
            else
              instruction.lifecycle.clear();
            changed = true;
          } else if (mutation == 2 &&
                     instruction.kind == lang::MirInstructionKind::CallInput &&
                     instruction.callInputKind ==
                         lang::HirCallInputKind::ReadTemporaryBorrow) {
            instruction.loan.reset();
            changed = true;
          }
        }
      }
    }
    if (!changed || lang::verifyMirProgram(forged).valid()) {
      std::cerr << "constant temporary forgery accepted: " << mutation << '\n';
      return 1;
    }
  }
  return 0;
}
