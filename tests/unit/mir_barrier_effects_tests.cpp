// A barrier query with an explicit body resolves call effects in that body,
// including when inspecting a copy of its instruction.
#include "gti/frontend.h"
#include "gti/optimization/coverage.h"
#include "unit_test.h"

#include <iostream>

GTI_UNIT_TEST(mir_barrier_effects) {
  using namespace lang;
  const auto frontend = Frontend().analyze("barrier.gti", R"(
int identity(int value) { return value; }
int main() { return identity(7); }
)");
  if (!frontend.canGenerateCode())
    return 1;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::Call)
          continue;
        const auto &body = function.body;
        const auto expected =
            mirOptimizationBarrierReason(frontend.mir, instruction, &body);
        const MirInstruction copy = instruction;
        if (mirOptimizationBarrierReason(frontend.mir, copy, &body) !=
            expected) {
          std::cerr << "Copying a call changed its barrier explanation\n";
          return 1;
        }
        return 0;
      }
    }
  }
  std::cerr << "Fixture must contain a call instruction\n";
  return 1;
}
