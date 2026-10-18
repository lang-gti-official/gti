#include "compiler_test_fixture.h"
#include "gti/mir_printer.h"

namespace {
lang::FrontendResult analyzeFresh(const std::string &source) {
  return lang::Frontend().analyze("fresh-callable-borrows.gti", source,
                                  {standardLibraryPrelude()}, {},
                                  {standardLibraryRoot()});
}

void positiveAndProofs() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/fresh_callable_borrows.gti";
  std::ifstream input(fixture);
  const std::string source((std::istreambuf_iterator<char>(input)), {});
  const auto frontend = analyzeFresh(source);
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.primary.line << ": " << diagnostic.message
                << '\n';
    const auto log =
        std::filesystem::temp_directory_path() / "gti-fresh-callable-proof.mir";
    std::ofstream output(log);
    lang::MirPrinter().print(frontend.mir, output);
    std::cerr << "Fresh callable proof MIR: " << log << '\n';
  }
  expect(frontend.canGenerateCode(),
         "fresh callable arguments preserve exact checked lifetimes");
  if (!frontend.canGenerateCode())
    return;
  std::size_t plain = 0;
  std::size_t borrowed = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &stage : block.instructions) {
        if (stage.kind != lang::MirInstructionKind::CallInput ||
            stage.callInputRole != lang::MirCallInputRole::Argument ||
            stage.callInputKind !=
                lang::HirCallInputKind::ReadTemporaryBorrow ||
            !stage.loan)
          continue;
        const auto *shell = function.body.findLoan(*stage.loan);
        const auto *place =
            shell ? function.body.findPlace(shell->source) : nullptr;
        if (!place || (place->type.kind != lang::SemanticType::Lambda &&
                       place->type.kind != lang::SemanticType::FunctionItem))
          continue;
        ++plain;
        borrowed += shell->storedDependency != 0;
        for (int mutation = 0; mutation != 8; ++mutation) {
          auto malformed = frontend.mir;
          auto *target = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto &loan = target->body.loans[shell->id - 1];
          bool changed = true;
          if (mutation == 0)
            loan.access = lang::AccessMode::Mutable;
          else if (mutation == 1)
            loan.escapes = true;
          else if (mutation == 2) {
            changed = loan.storedDependency != 0;
            loan.storedDependency = 0;
          } else if (mutation == 3) {
            changed = false;
            for (auto &drop : target->body.dropObligations)
              if (drop.place == place->id) {
                drop.fullExpression = 0;
                changed = true;
              }
          } else if (mutation == 4) {
            changed = loan.storedDependency != 0;
            if (changed) {
              auto &capture = target->body.loans[loan.storedDependency - 1];
              capture.designatedFields.clear();
            }
          } else if (mutation == 5) {
            ++loan.producedBy;
          } else if (mutation == 6) {
            const auto materialization = lang::findMirTemporaryMaterialization(
                target->body, *target->body.findPlace(place->id),
                stage.hirValue);
            changed = materialization && materialization.producer->lambdaTarget;
            if (changed) {
              auto *producer =
                  const_cast<lang::MirInstruction *>(materialization.producer);
              ++*producer->lambdaTarget;
            }
          } else {
            changed = false;
            if (loan.storedDependency) {
              auto &capture = target->body.loans[loan.storedDependency - 1];
              const auto *origin = target->body.findPlace(capture.source);
              for (const auto &replacement : target->body.places) {
                if (origin && replacement.id != origin->id &&
                    (replacement.root == lang::MirPlaceRootKind::Symbol ||
                     replacement.root == lang::MirPlaceRootKind::Binding) &&
                    replacement.type == origin->type) {
                  capture.source = replacement.id;
                  changed = true;
                  break;
                }
              }
            }
          }
          if (!changed)
            continue;
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "fresh callback proof rejects forged access, escape, capture "
                 "dependency or lifetime");
        }
      }
    }
  }
  expect(plain >= 5 && borrowed >= 2,
         "lambda, function item, multi-capture and retained-view cases have "
         "explicit call loans");
}

void negativeBoundaries() {
  const std::string prefix = R"gti(
int invoke<F>(F& callback, int value) { int result = callback(value); return result; }
int mutable_invoke<F>(mut F& callback) { return callback(1); }
F& escape<F>(F& callback) { return callback; }
int change(mut int& value) { value += 1; return value; }
int increment(int value) { return value + 1; }
int unproven<F>(F& callback) { return 0; }
)gti";
  for (const std::string body :
       {"return unproven(increment);",
        "return mutable_invoke([](int value) -> int { return value; });",
        "auto& escaped = escape([](int value) -> int { return value; }); "
        "return escaped(1);",
        "auto callback = [](int value) -> int { return value; }; return "
        "invoke(std::move(callback), 1);",
        "mut int state = 1; return invoke([mut &state](int value) -> int { "
        "state += value; return state; }, 1);",
        "mut int state = 1; return invoke([&state](int value) -> int { return "
        "state + value; }, change(state));"}) {
    const auto result = analyzeFresh(prefix + "int main() { " + body + " }");
    expect(!result.canGenerateCode() &&
               std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.phase ==
                                        lang::DiagnosticPhase::Semantics &&
                                    diagnostic.severity ==
                                        lang::DiagnosticSeverity::Error;
                           }),
           "temporary callables cannot escape, borrow mutably, move existing "
           "storage or alias captures");
  }
}
} // namespace

int main() {
  positiveAndProofs();
  negativeBoundaries();
  return failures == 0 ? 0 : 1;
}
