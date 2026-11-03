#include "compiler_test_fixture.h"

namespace {
lang::FrontendResult analyze(std::string source) {
  return lang::Frontend().analyze("scalar-temporary-borrow.gti", std::move(source),
                                 {standardLibraryPrelude()}, {},
                                 {standardLibraryRoot()});
}
void accepted(std::string source, std::string label) {
  const auto result = analyze(std::move(source));
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << label << ": " << diagnostic.code << ": "
                << diagnostic.message << '\n';
  if (!result.canGenerateCode() && true) {
    std::ofstream log("/private/tmp/gti-161-operator-" + label + ".mir");
    lang::MirPrinter().print(result.mir, log);
  }
  expect(result.canGenerateCode(), label);
  if (result.canGenerateCode())
    expect(lang::verifyMirProgram(result.mir).valid(), label + " verifies MIR");
}
void rejected(std::string source, std::string label,
              std::string_view code = "GTI-S2017") {
  const auto result = analyze(std::move(source));
  expect(!result.canGenerateCode(), label);
  expect(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const auto &diagnostic) { return diagnostic.code == code; }),
         label + " reports its source diagnostic");
}
const std::string scalar = R"gti(
enum Color { Red, Green };
int read(int& value) { return value; }
int make() { return 42; }
Color pick() { return Color::Red; }
bool red(Color& value) { return value == Color::Red; }
)gti";
void scalarValues() {
  for (const std::string expression :
       {"42", "make()", "int(42)", "21 + 21", "-(-42)",
        "(true ? 42 : make())", "((42))"})
    accepted(scalar + "int main() { return read(" + expression + ") - 42; }",
             "scalar producer " + expression);
  accepted(scalar + "int main() { return red(pick()) ? 0 : 1; }",
           "plain enum factory");
  accepted(R"gti(
bool read(bool& value) { return value; }
char read(char& value) { return value; }
float read(float& value) { return value; }
int main() { return read(true && false) || read('a') != 'a' || read(float(1.0)) != float(1.0) ? 1 : 0; }
)gti", "boolean character and floating temporaries");
  accepted(scalar + "int main() { int value = 42; return read(std::move(value)) - 42; }",
           "explicitly moved scalar");
  accepted("int measure(std::string_view& value) { return int(value.size()); } "
           "int main() { return measure(\"abcd\") - 4; }", "literal text temporary");
  accepted(scalar + "int main() { bool skip = false; return skip && read(1 / (make() - 42)) == 1 ? 1 : 0; }",
           "lazy scalar temporary is path-local");
}
void boundaries() {
  rejected("void write(mut int& value) { value = 1; } int main() { write(42); return 0; }",
           "mutable scalar reference");
  rejected("int& relay(int& value) { return value; } int main() { return relay(42); }",
           "reference result cannot retain temporary");
  rejected("int read(int& value); int main() { return read(42); }",
           "declaration-only target");
  rejected(scalar + "int main() { int value = 42; int answer = read(std::move(value)); return value; }",
           "move consumes scalar local", "GTI-S2045");
  accepted("[[borrowed_from(source)]] int& select(int& source, int& ignored) { return source; } "
           "int main() { int source = 42; return select(source, 0) - 42; }",
           "result may retain a different argument");
  accepted("int choose(int& value) { return 1; } int choose(int value) { return 2; } "
           "int main() { return choose(42) - 2; }", "exact value tier wins");
  accepted("int choose(int& value) { return 1; } int choose(int value) { return 2; } "
           "int main() { int n = 42; return choose(n) - 2; }",
           "ordinary function conversion ranking is unchanged");
}
void operators() {
  accepted(R"gti(
class Tag { public: int value; Tag(int v) : value(v) {} bool operator==(Tag& other) { return this.value == other.value; } };
int main() { Tag a{7}; return a == Tag(7) ? 0 : 1; }
)gti", "class operator temporary argument");
  accepted(R"gti(
class Counter { public: int value; Counter(int v) : value(v) {} int operator+(int& other) { return this.value + other; } };
int main() { Counter a{7}; return (a + 35) - 42; }
)gti", "scalar operator temporary argument");
  accepted(R"gti(
class Cursor { int& source; public: Cursor(int& value) : source(value) {} bool operator==(Cursor& other) { return this.source == other.source; } };
Cursor cursor(int& source) { return Cursor(source); }
int main() { int owner = 7; return cursor(owner) == cursor(owner) ? 0 : 1; }
)gti", "temporary cursor preserves stable external origin");
  rejected(R"gti(
class Tag { public: Tag() = default; bool operator==(mut Tag& other) { return true; } };
int main() { Tag a{}; return a == Tag() ? 0 : 1; }
)gti", "mutable operator argument", "GTI-S2022");
  const auto ambiguity = analyze(R"gti(
class Tag { public: Tag() = default; bool operator==(Tag& other) { return true; } bool operator==(Tag other) { return true; } };
int main() { Tag a{}; Tag b{}; return a == b ? 0 : 1; }
)gti");
  expect(!ambiguity.canGenerateCode(), "operator exact ambiguity preserved");
  expect(std::any_of(ambiguity.diagnostics.begin(), ambiguity.diagnostics.end(),
                    [](const auto &diagnostic) {
                      return diagnostic.code == "GTI-S2022" &&
                             diagnostic.related.size() >= 2 && !diagnostic.hints.empty();
                    }), "operator ambiguity lists candidates and remedy");
}
void libraryClients() {
  accepted(R"gti(
import std.set;
import std.vector;
import std.algorithm;
int main() {
  mut std::set<int> values{}; values.insert(80);
  mut std::vector<int> sequence{}; sequence.push_back(2); sequence.push_back(3);
  auto found = std::find(sequence.begin(), sequence.end(), 3);
  return values.contains(80) && found != sequence.end() && std::count(sequence, 2) == 1 ? 0 : 1;
}
)gti", "set lookup and iterator algorithms");
}
void verifierProofs() {
  auto frontend = analyze(scalar + "int main() { return read(42) - 42; }");
  if (!frontend.canGenerateCode()) return;
  bool checked = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance>&>(frontend.mir.functionInstances())) {
    for (auto &block : function.body.blocks) {
      for (auto &input : block.instructions) {
        if (input.callInputKind != lang::HirCallInputKind::ReadTemporaryBorrow ||
            input.callInputRole != lang::MirCallInputRole::Argument || !input.loan)
          continue;
        auto &loan = function.body.loans[*input.loan - 1];
        loan.escapes = true;
        expect(!lang::verifyMirProgram(frontend.mir).valid(),
               "forged escaping scalar temporary loan rejected");
        checked = true;
        loan.escapes = false;
      }
    }
  }
  expect(checked, "scalar borrow has explicit verifiable loan");
}
}
int main() {
  scalarValues(); boundaries(); operators(); libraryClients(); verifierProofs();
  return failures ? 1 : 0;
}
