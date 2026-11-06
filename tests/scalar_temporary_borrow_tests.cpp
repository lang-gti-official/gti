#include "compiler_test_fixture.h"
#include "gti/mir_editor.h"

namespace {
lang::FrontendResult analyze(std::string source) {
  return lang::Frontend().analyze("scalar-temporary-borrow.gti",
                                  std::move(source), {standardLibraryPrelude()},
                                  {}, {standardLibraryRoot()});
}
void accepted(std::string source, std::string label) {
  const auto result = analyze(std::move(source));
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << label << ": " << diagnostic.code << ": "
                << diagnostic.message << '\n';
  expect(result.canGenerateCode(), label);
  if (result.canGenerateCode())
    expect(lang::verifyMirProgram(result.mir).valid(), label + " verifies MIR");
}
void rejected(std::string source, std::string label,
              std::string_view code = "GTI-S2017") {
  const auto result = analyze(std::move(source));
  expect(!result.canGenerateCode(), label);
  expect(std::any_of(
             result.diagnostics.begin(), result.diagnostics.end(),
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
       {"42", "make()", "int(42)", "21 + 21", "-(-42)", "(true ? 42 : make())",
        "((42))"})
    accepted(scalar + "int main() { return read(" + expression + ") - 42; }",
             "scalar producer " + expression);
  accepted(scalar + "int main() { return red(pick()) ? 0 : 1; }",
           "plain enum factory");
  // A call's fresh result follows its resolved declaration wherever the body
  // lives: interface dispatch and foreign functions publish it the same way.
  accepted(scalar + R"gti(
interface meter { int measure(); };
class gauge : public meter {
  int value;
public:
  gauge(int value) : value(value) {}
  int measure() override { return this.value; }
};
int main() { gauge owner = gauge(42); meter& view = owner; return read(view.measure()) - 42; }
)gti",
           "dispatched interface result");
  accepted(scalar + R"gti(
extern "C" { int abs(int value); }
int main() { return read(abs(-42)) - 42; }
)gti",
           "foreign function result");
  accepted(R"gti(
bool read(bool& value) { return value; }
char read(char& value) { return value; }
float read(float& value) { return value; }
int main() { return read(true && false) || read('a') != 'a' || read(float(1.0)) != float(1.0) ? 1 : 0; }
)gti",
           "boolean character and floating temporaries");
  accepted(
      scalar +
          "int main() { int value = 42; return read(std::move(value)) - 42; }",
      "explicitly moved scalar");
  accepted("int measure(std::string_view& value) { return int(value.size()); } "
           "int main() { return measure(\"abcd\") - 4; }",
           "literal text temporary");
  accepted("import std.string; int measure(std::string_view& value) { return "
           "int(value.size()); } "
           "int main() { std::string owner{\"abcd\"}; return "
           "measure(owner.view()) - 4; }",
           "text view temporary retains stable owner");
  accepted(scalar + "int main() { bool skip = false; return skip && read(1 / "
                    "(make() - 42)) == 1 ? 1 : 0; }",
           "lazy scalar temporary is path-local");
}
void boundaries() {
  rejected("import std.string; int measure(std::string_view& value) { return "
           "int(value.size()); } "
           "int main() { return measure(std::string(\"abcd\").view()); }",
           "text view temporary cannot retain transient owner");
  rejected("void write(mut int& value) { value = 1; } int main() { write(42); "
           "return 0; }",
           "mutable scalar reference");
  rejected("int& relay(int& value) { return value; } int main() { return "
           "relay(42); }",
           "reference result cannot retain temporary");
  rejected("int read(int& value); int main() { return read(42); }",
           "declaration-only target");
  rejected(scalar + "int main() { int value = 42; int answer = "
                    "read(std::move(value)); return value; }",
           "move consumes scalar local", "GTI-S2018");
  accepted("[[borrowed_from(source)]] int& select(int& source, int& ignored) { "
           "return source; } "
           "int main() { int source = 42; return select(source, 0) - 42; }",
           "result may retain a different argument");
  accepted("int choose(int& value) { return 1; } int choose(int value) { "
           "return 2; } "
           "int main() { return choose(42) - 2; }",
           "exact value tier wins");
  accepted("int choose(int& value) { return 1; } int choose(int value) { "
           "return 2; } "
           "int main() { int n = 42; return choose(n) - 2; }",
           "ordinary function conversion ranking is unchanged");
}
void operators() {
  accepted(R"gti(
class Tag { public: int value; Tag(int v) : value(v) {} bool operator==(Tag& other) { return this.value == other.value; } };
int main() { Tag a{7}; return a == Tag(7) ? 0 : 1; }
)gti",
           "class operator temporary argument");
  accepted(R"gti(
class Counter { public: int value; Counter(int v) : value(v) {} int operator+(int& other) { return this.value + other; } };
int main() { Counter a{7}; return (a + 35) - 42; }
)gti",
           "scalar operator temporary argument");
  accepted(R"gti(
class Cursor { int& source; public: Cursor(int& value) : source(value) {} bool operator==(Cursor& other) { return this.source == other.source; } };
Cursor cursor(int& source) { return Cursor(source); }
int main() { int owner = 7; return cursor(owner) == cursor(owner) ? 0 : 1; }
)gti",
           "temporary cursor preserves stable external origin");
  rejected(R"gti(
class Tag { public: Tag() = default; bool operator==(mut Tag& other) { return true; } };
int main() { Tag a{}; return a == Tag() ? 0 : 1; }
)gti",
           "mutable operator argument", "GTI-S2022");
  const auto ambiguity = analyze(R"gti(
class Tag { public: Tag() = default; bool operator==(Tag& other) { return true; } bool operator==(Tag other) { return true; } };
int main() { Tag a{}; Tag b{}; return a == b ? 0 : 1; }
)gti");
  expect(!ambiguity.canGenerateCode(), "operator exact ambiguity preserved");
  expect(std::any_of(ambiguity.diagnostics.begin(), ambiguity.diagnostics.end(),
                     [](const auto &diagnostic) {
                       return diagnostic.code == "GTI-S2022" &&
                              diagnostic.related.size() >= 2 &&
                              !diagnostic.hints.empty();
                     }),
         "operator ambiguity lists candidates and remedy");
}
void carrierBoundaries() {
  const std::string declarations = R"gti(
class Cursor { int& source; public: Cursor(int& value) : source(value) {} int read() { return this.source; } };
Cursor cursor(int& source) { return Cursor(source); }
int mutate(mut int& owner) { owner = 8; return owner; }
int observe(Cursor& cursor, int later) { return cursor.read() + later; }
)gti";
  rejected(declarations + "int main() { return observe(cursor(42), 0); }",
           "temporary carrier cannot retain a temporary owner");
  const auto conflict =
      analyze(declarations + "int main() { mut int owner = 7; return "
                             "observe(cursor(owner), mutate(owner)); }");
  expect(!conflict.canGenerateCode() &&
             std::none_of(conflict.diagnostics.begin(),
                          conflict.diagnostics.end(),
                          [](const auto &diagnostic) {
                            return diagnostic.code == "GTI-B0001";
                          }),
         "carrier argument protects its owner during later arguments");
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
)gti",
           "set lookup and iterator algorithms");
}
void verifierProofs() {
  auto frontend = analyze(scalar + "int main() { return read(42) - 42; }");
  if (!frontend.canGenerateCode())
    return;
  bool checked = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           frontend.mir.functionInstances())) {
    for (auto &block : function.body.blocks) {
      for (auto &input : block.instructions) {
        if (input.callInputKind !=
                lang::HirCallInputKind::ReadTemporaryBorrow ||
            input.callInputRole != lang::MirCallInputRole::Argument ||
            !input.loan)
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
  const auto lazy = analyze(scalar + "int main() { bool run = true; return run "
                                     "&& read(make()) == 42 ? 0 : 1; }");
  expect(lazy.canGenerateCode(), "conditional scalar lifetime control");
  if (lazy.canGenerateCode()) {
    auto forged = lazy.mir;
    bool removed = false;
    for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
             forged.functionInstances()))
      for (auto &block : function.body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.kind == lang::MirInstructionKind::Call &&
              instruction.info.type == lang::SemanticType::Int32 &&
              !instruction.lifecycle.empty()) {
            instruction.lifecycle.clear();
            removed = true;
          }
    expect(removed && !lang::verifyMirProgram(forged).valid(),
           "conditional scalar borrow cannot bypass its exact initialization "
           "obligation");
  }
}

void primitiveBorrowInitialization() {
  struct Case {
    std::string type;
    std::string value;
    std::string read;
    lang::SemanticType::Kind kind;
  };
  for (const auto &item : std::vector<Case>{
           {"int", "1", "alias - 1", lang::SemanticType::Int32},
           {"bool", "true", "alias ? 0 : 1", lang::SemanticType::Bool},
           {"char", "'a'", "alias == 'a' ? 0 : 1", lang::SemanticType::Char},
           {"float", "1.0", "int(alias) - 1", lang::SemanticType::Float},
           {"double", "double(1.0)", "int(alias) - 1",
            lang::SemanticType::Double},
           {"int*", "nullptr", "alias == nullptr ? 0 : 1",
            lang::SemanticType::RawPointer},
           {"nullptr_t", "nullptr", "alias == nullptr ? 0 : 1",
            lang::SemanticType::NullPtr}}) {
    for (const std::string access : {"", "mut ", "copy"}) {
      const std::string alias = access == "copy"
                                    ? item.type + " alias"
                                    : access + item.type + "& alias";
      auto result =
          analyze("int main() { mut " + item.type + " value = " + item.value +
                  "; " + alias + " = value; return " + item.read + "; }");
      const std::string label =
          access + item.type + " initialized local borrow";
      expect(result.canGenerateCode(), label);
      if (!result.canGenerateCode()) {
        for (const auto &diagnostic : result.diagnostics)
          std::cerr << label << ": " << diagnostic.message << '\n';
        continue;
      }
      bool erased = false;
      for (auto &function :
           const_cast<std::vector<lang::MirFunctionInstance> &>(
               result.mir.functionInstances())) {
        const auto *info = result.semantics.findFunction(function.declaration);
        if (!info || !info->declaration ||
            info->declaration->name().lexeme != "main")
          continue;
        for (auto &block : function.body.blocks) {
          const auto initializer = std::find_if(
              block.instructions.begin(), block.instructions.end(),
              [&](const auto &instruction) {
                const auto *place =
                    instruction.destination
                        ? function.body.findPlace(*instruction.destination)
                        : nullptr;
                return instruction.kind ==
                           lang::MirInstructionKind::Initialize &&
                       place && place->type.kind == item.kind &&
                       place->root == lang::MirPlaceRootKind::Binding &&
                       place->projections.empty();
              });
          if (initializer != block.instructions.end()) {
            block.instructions.erase(initializer);
            erased = true;
            break;
          }
        }
        (void)lang::rebuildMirValueUses(function.body);
      }
      const auto proof = lang::verifyMirProgram(result.mir);
      expect(
          erased && !proof.valid() &&
              std::any_of(proof.errors.begin(), proof.errors.end(),
                          [](const auto &error) {
                            return error.message.find(
                                       "definitely initialized scalar place") !=
                                   std::string::npos;
                          }),
          label + " rejects removed initialization for the intended reason");
    }
  }
}

void sharedParentCallArguments() {
  const std::string declarations =
      "int read(int& value) { return value; } "
      "void keep(int& value) {} void write(mut int& value) { value += 1; } ";
  accepted(declarations +
               "int main() { mut int owner = 1; mut int& parent = owner; "
               "int& child = parent; keep(parent); keep(child); return 0; }",
           "shared parent call beside a live read-only child (#286)");
  accepted(declarations +
               "int main() { mut int owner = 1; mut int& parent = owner; "
               "int& child = parent; int answer = read(parent); return answer "
               "+ child - 2; }",
           "shared parent call returns a scalar beside a live child");
  accepted(declarations +
               "int via(mut int& parent) { int& child = parent; "
               "int answer = read(parent); return answer + child; } "
               "int main() { mut int value = 1; return via(value) - 2; }",
           "shared call through an incoming mutable reference");
  accepted(declarations +
               "int main() { mut int owner = 1; mut int& parent = owner; "
               "mut int& child = parent; keep(child); write(parent); return "
               "owner - 2; }",
           "mutable parent call after child endpoint");
  rejected(declarations +
               "int main() { mut int owner = 1; mut int& parent = owner; "
               "int& child = parent; write(parent); keep(child); return 0; }",
           "parent write remains forbidden beside a shared child");
  rejected(
      declarations +
          "int main() { mut int owner = 1; mut int& parent = owner; "
          "mut int& child = parent; keep(parent); write(child); return 0; }",
      "parent read remains forbidden beside an exclusive child");
}

void mixedArgumentLoanOrder() {
  const std::string declarations = R"gti(
struct Holder {
  int value;
  Holder(int value) : value(value) {}
  int& get() { return this.value; }
};
int divide(int& left, int& right) { return left / right; }
int combine(int& a, int& b, int& c, int& d) { return a / b + c / d; }
)gti";
  auto result = analyze(declarations + R"gti(
int main() { Holder h{2}; return divide(42, h.get()); }
)gti");
  expect(
      result.canGenerateCode(),
      "later reference argument cannot precede temporary-input failure ends");
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return;
  }
  bool mutated = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           result.mir.functionInstances())) {
    for (const auto &block : function.body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Invoke ||
          block.instructions.empty())
        continue;
      const auto &call = block.instructions.back();
      if (call.kind != lang::MirInstructionKind::Call ||
          call.operands.size() != 2)
        continue;
      auto &failure =
          function.body.blocks[block.terminator.elseTarget - 1].instructions;
      if (failure.size() < 2 ||
          failure[0].kind != lang::MirInstructionKind::EndBorrow ||
          failure[1].kind != lang::MirInstructionKind::EndBorrow)
        continue;
      std::swap(failure[0], failure[1]);
      expect(!lang::verifyMirProgram(result.mir).valid(),
             "failure cleanup cannot retire a later reference before the "
             "invocation's temporary input");
      std::swap(failure[0], failure[1]);
      mutated = true;
    }
  }
  expect(mutated, "mixed argument failure epilogue was independently checked");
  accepted(declarations + R"gti(
struct Use {
  int result;
  Use(Holder& h) : result(combine(42, h.get(), 6, h.get())) {}
};
int main() { Holder h{2}; Use result{h}; return result.result - 24; }
)gti",
           "constructor initializer interleaves multiple temporary and "
           "reference arguments");
}
} // namespace
int main() {
  scalarValues();
  boundaries();
  operators();
  carrierBoundaries();
  libraryClients();
  verifierProofs();
  primitiveBorrowInitialization();
  sharedParentCallArguments();
  mixedArgumentLoanOrder();
  return failures ? 1 : 0;
}
