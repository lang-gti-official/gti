#include "compiler_test_fixture.h"
#include "gti/mir_editor.h"

int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path();
  std::ifstream input(root / "fixtures/bounded_array_borrows.gti");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto valid = analyzeTrustedPreludeFixture("array-borrows.gti", source);
  for (const auto &diagnostic : valid.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(valid.canGenerateCode(), "general array borrows reach verified MIR");
  const auto formatted = lang::Formatter().format(source);
  expect(lang::Formatter().format(formatted) == formatted,
         "array-reference syntax formats idempotently");
  expect(analyzeTrustedPreludeFixture("formatted-arrays.gti", formatted)
             .canGenerateCode(),
         "formatting preserves array-reference meaning");
  if (valid.canGenerateCode()) {
    expect(lang::verifyMirProgram(valid.mir).valid(), "array loans verify");
    bool checked = false;
    for (const auto &function : valid.mir.functionInstances()) {
      for (const auto &loan : function.body.loans) {
        if (loan.kind != lang::MirLoanKind::Local || loan.source == 0)
          continue;
        const auto *origin = function.body.findPlace(loan.source);
        if (!origin || origin->type.kind != lang::SemanticType::Array)
          continue;
        auto detached = valid.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            detached.findFunctionInstance(function.id));
        changed->body.loans[loan.id - 1].source = 0;
        expect(!lang::verifyMirProgram(detached).valid(),
               "array loan cannot lose its owner/address proof");
        checked = true;
      }
    }
    expect(checked, "fixture exercises retained array loans");
    bool capturedChecked = false;
    for (const auto &lambda : valid.mir.lambdaInstances()) {
      for (const auto &block : lambda.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::CallInput ||
              instruction.operands.size() != 1 ||
              instruction.operands[0].kind != lang::MirOperandKind::Loan ||
              instruction.operands[0].place == 0)
            continue;
          for (const auto &other : lambda.body.loans) {
            if (!other.entry || other.kind != lang::MirLoanKind::Parameter ||
                other.id == instruction.operands[0].loan)
              continue;
            auto forged = valid.mir;
            auto *body = lang::findMirBody(
                forged, {lang::MirBodyKind::Lambda, lambda.id});
            for (auto &changedBlock : body->blocks)
              for (auto &changed : changedBlock.instructions)
                if (changed.id == instruction.id)
                  changed.operands[0].loan = other.id;
            expect(
                !lang::verifyMirProgram(forged).valid(),
                "same-typed capture cannot borrow one array and pass another");
            capturedChecked = true;
          }
        }
      }
    }
    expect(capturedChecked, "fixture checks distinct captured array origins");
  }
  const std::vector<std::pair<std::string, std::string>> rejected = {
      {"extent", "void take(int[2]& x) {} int main() { int a[3] = {1,2,3}; "
                 "take(a); return 0; }"},
      {"element type", "void take(float[2]& x) {} int main() { int a[2] = "
                       "{1,2}; take(a); return 0; }"},
      {"read only", "void change(int[2]& x) { x[0] = 3; }"},
      {"mutable upgrade",
       "int main() { int a[2] = {1,2}; mut int[2]& r = a; return r[0]; }"},
      {"parent upgrade",
       "void change(int[2]& x) { mut int[2]& r = x; r[0] = 3; }"},
      {"overlap", "int main() { mut int a[2] = {1,2}; mut int[2]& x = a; mut "
                  "int[2]& y = a; return x[0] + y[1]; }"},
      {"owner mutation", "int main() { mut int a[2] = {1,2}; int[2]& r = a; "
                         "a[1] = 3; return r[0]; }"},
      {"element overlap", "int main() { mut int a[2] = {1,2}; mut int& e = "
                          "a[0]; mut int[2]& r = a; return e + r[1]; }"},
      {"local escape", "int[2]& escape() { int a[2] = {1,2}; return a; }"},
      {"nested escape",
       "int[2]& escape() { int a[2][2] = {{1,2},{3,4}}; return a[0]; }"},
      {"temporary", "int[2] make() { return {1,2}; } int main() { int[2]& r = "
                    "make(); return r[0]; }"},
      {"call alias", "void change(mut int[2]& x, int[2]& y) {} int main() { "
                     "mut int a[2] = {1,2}; change(a,a); return 0; }"},
      {"constant bounds", "int read(int[2]& a) { return a[2]; }"},
  };
  for (const auto &[name, program] : rejected) {
    const auto result = analyzeTrustedPreludeFixture(name + ".gti", program);
    expect(result.syntaxValid, name + " uses valid existing syntax");
    expect(!result.semanticValid, name + " rejected by semantics");
    expect(
        std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [](const auto &d) { return d.code.starts_with("GTI-B"); }),
        name + " does not reach backend failure");
  }
  return failures == 0 ? 0 : 1;
}
