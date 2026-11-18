// Composed reference results preserve every owner and the declared permission.
#include "gti/frontend.h"
#include "gti/mir_verifier.h"
#include "unit_test.h"

GTI_UNIT_TEST(composed_reference_permissions) {
  using namespace lang;
  const auto frontend = Frontend().analyze("composed.gti", R"(
int&<a> choose<lifetime a>(mut int&<a> one, mut int&<a> two, bool first) {
  if (first) { return one; }
  return two;
}
int main() { mut int one = 1; mut int two = 2; return choose(one, two, true); }
)");
  if (!frontend.canGenerateCode())
    return 1;
  for (int mutation = 0; mutation != 3; ++mutation) {
    auto forged = frontend.mir;
    bool changed = false;
    auto &functions = const_cast<std::vector<MirFunctionInstance> &>(
        forged.functionInstances());
    for (auto &function : functions) {
      for (auto &loan : function.body.loans) {
        if (loan.kind != MirLoanKind::CallResult ||
            loan.additionalDependencies.empty())
          continue;
        if (mutation == 0)
          loan.additionalDependencies.clear();
        if (mutation == 1)
          loan.additionalDependencies.front().access = AccessMode::Mutable;
        if (mutation == 2)
          loan.access = AccessMode::Mutable;
        changed = true;
      }
    }
    if (!changed || verifyMirProgram(forged).valid())
      return 1;
  }
  return 0;
}
