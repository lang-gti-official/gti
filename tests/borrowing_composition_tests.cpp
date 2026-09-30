#include "compiler_test_fixture.h"

namespace {

void checkIndependentDependencies() {
  const auto frontend = lang::Frontend().analyze("composed-borrows.gti", R"(
struct View {
  int& first;
  int& second;
  View(int& a, int& b) : first(a), second(b) {}
  int sum() { return first + second; }
};
View relay(View value) { return value; }
View factory(int& first, int& second) { return View(first, second); }
int main() {
  int a = 20; int b = 22;
  View view{a, b};
  auto copy = view;
  return copy.sum() - 42;
}
)");
  expect(frontend.canGenerateCode(),
         "user-defined two-owner carrier should pass complete MIR lowering");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "composed owner dependencies should independently verify");
  const auto semantic = std::find_if(
      frontend.semantics.loans().begin(), frontend.semantics.loans().end(),
      [](const lang::SemanticLoanInfo &loan) {
        return !loan.entry && !loan.additionalDependencies.empty();
      });
  expect(semantic != frontend.semantics.loans().end(),
         "semantic loan must retain the second independent place");
  if (semantic != frontend.semantics.loans().end()) {
    expect(semantic->place.root !=
               semantic->additionalDependencies.front().place.root,
           "independent owners must not collapse into a fabricated root");
    expect(semantic->carriers.size() == 2,
           "copy construction must retain both aliases of the dependency set");
  }
  bool inspected = false;
  bool inspectedPaths = false;
  bool inspectedEffects = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (!function.returnBorrowParameters.empty()) {
      inspectedEffects = true;
      expect(function.effectSummary.argumentsEscape ==
                 std::vector<bool>({true, true}),
             "effect summaries must retain both zero-based parameter escapes");
    }
    for (const auto &loan : function.body.loans) {
      if (loan.entry && !loan.additionalDesignatedFields.empty()) {
        inspectedPaths = true;
        auto malformed = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        instance->body.loans[loan.id - 1].additionalDesignatedFields.clear();
        expect(
            !lang::verifyMirProgram(malformed).valid(),
            "MIR must reject dropping an opaque parameter's second field path");
      }
      if (loan.additionalSources.empty()) {
        continue;
      }
      inspected = true;
      auto malformed = frontend.mir;
      auto *instance = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      instance->body.loans[loan.id - 1].additionalSources.clear();
      expect(!lang::verifyMirProgram(malformed).valid(),
             "MIR must reject erasing an independent retained owner");
      malformed = frontend.mir;
      instance = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      instance->body.loans[loan.id - 1].additionalSources.front() = 0;
      expect(!lang::verifyMirProgram(malformed).valid(),
             "MIR must reject an unresolved independent owner place");
      break;
    }
  }
  expect(inspected, "MIR must retain an explicit multi-owner dependency set");
  expect(inspectedPaths, "MIR must retain all opaque parameter field paths");
  expect(inspectedEffects, "factory must publish its complete escape summary");
}

constexpr NamedTest kTests[] = {
    {"independent_dependencies", checkIndependentDependencies},
};

} // namespace

int main(int argc, char **argv) {
  lang::installCrashHandlers("gti_borrowing_composition_tests");
  return runNamedTests(argc, argv, kTests, "borrowing composition");
}
