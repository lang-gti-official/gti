#include "compiler_test_fixture.h"
#include "gti/hir.h"
#include "gti/mir_verifier.h"

namespace {
void printDiagnostics(const lang::FrontendResult &result) {
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
}

lang::FrontendResult analyzeText(std::string source) {
  return lang::Frontend().analyze("view-fields.gti", std::move(source),
                                  {standardLibraryPrelude()}, {},
                                  {standardLibraryRoot()});
}

const std::string kDeclarations = R"gti(
#include <std/string>
class Token {
public:
  std::string_view text;
  int kind;
  Token(std::string_view text, int kind) : text(text), kind(kind) {}
  uint64_t width() { return this.text.size(); }
};
struct Diag {
  std::string_view text;
  int line;
};
class Pair {
public:
  std::string_view first;
  std::string_view second;
  Pair(std::string_view first, std::string_view second)
      : first(first), second(second) {}
};
class Logged {
public:
  std::string_view text;
  Logged(std::string_view text) : text(text) {}
  ~Logged() {}
};
mut std::string_view kept = "";
uint64_t measure(Token token) { return token.text.size(); }
uint64_t inspect(Token& token) { return token.text.size(); }
)gti";

struct Location {
  lang::HirFunctionInstanceId function = 0;
  std::size_t block = 0;
  std::size_t instruction = 0;
};

// Every construction that publishes dynamic text, in body order.
std::vector<Location> publications(const lang::MirProgram &program) {
  std::vector<Location> found;
  for (const auto &function : program.functionInstances())
    for (std::size_t block = 0; block < function.body.blocks.size(); ++block)
      for (std::size_t index = 0;
           index < function.body.blocks[block].instructions.size(); ++index)
        if (function.body.blocks[block]
                .instructions[index]
                .dependentTextPublication)
          found.push_back({function.id, block, index});
  return found;
}

lang::MirInstruction &instructionAt(lang::MirProgram &program,
                                    const Location &location) {
  auto *function = const_cast<lang::MirFunctionInstance *>(
      program.findFunctionInstance(location.function));
  return function->body.blocks[location.block]
      .instructions[location.instruction];
}

void testDependentPublication() {
  const auto result = analyzeText(kDeclarations + R"gti(
int main() {
  std::string input{"token text"};
  Token token = Token(input.view(), 3);
  Diag diag{.text = input.view(), .line = 2};
  Token copy = token;
  Token moved = std::move(copy);
  std::string_view head = moved.text;
  Pair pair = Pair(input.view(), "fixed");
  Token literal = Token("literal", 1);
  kept = literal.text;
  kept = pair.second;
  return head.size() + diag.text.size() + literal.width() == 27 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "constructors and designated lists publish dynamic text locally");
  if (!result.canGenerateCode())
    return;
  const auto published = publications(result.mir);
  expect(published.size() == 3,
         "the token, diagnostic and pair constructions publish their text");
  for (const auto &location : published) {
    const auto *function = result.mir.findFunctionInstance(location.function);
    const auto &instruction = function->body.blocks[location.block]
                                  .instructions[location.instruction];
    const auto *loan =
        instruction.loan ? function->body.findLoan(*instruction.loan) : nullptr;
    expect(loan != nullptr && loan->kind == lang::MirLoanKind::Stored &&
               loan->access == lang::AccessMode::ReadOnly &&
               loan->designatedFields.size() == 1 &&
               instruction.borrowFields.size() == 1 &&
               loan->designatedFields ==
                   instruction.borrowFields.front().resultFields,
           "a publication's result loan designates its one text field");
  }
  // The dependency belongs to each call, never to the constructor itself.
  std::size_t textConstructors = 0;
  for (const auto &constructor : result.hir.constructorInstances()) {
    if (constructor.source == nullptr ||
        (constructor.source->name().lexeme != "Token" &&
         constructor.source->name().lexeme != "Pair"))
      continue;
    ++textConstructors;
    expect(constructor.borrowOrigin == lang::BorrowOriginKind::None,
           "a text-storing constructor keeps an empty borrow summary");
  }
  expect(textConstructors == 2, "both text-storing constructors are lowered");

  if (published.empty())
    return;
  {
    auto changed = result.mir;
    instructionAt(changed, published.front()).dependentTextPublication = false;
    expect(!lang::verifyMirProgram(changed).valid(),
           "dynamic text without its publication is not ownerless");
  }
  {
    auto changed = result.mir;
    auto &instruction = instructionAt(changed, published.front());
    instruction.borrowArgument = 1;
    instruction.borrowFields.front().parameter = 1;
    expect(!lang::verifyMirProgram(changed).valid(),
           "a publication must name the formal that initializes its field");
  }
  {
    auto changed = result.mir;
    auto &instruction = instructionAt(changed, published.front());
    auto *function = const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(published.front().function));
    auto *loan =
        const_cast<lang::MirLoan *>(function->body.findLoan(*instruction.loan));
    loan->designatedFields.clear();
    expect(!lang::verifyMirProgram(changed).valid(),
           "a publication's loan must designate its text field");
  }
}

void testOwnerlessConstructionUnchanged() {
  const auto result = analyzeText(kDeclarations + R"gti(
int main() {
  Token token = Token("literal", 1);
  Diag diag{.text = "message", .line = 2};
  kept = token.text;
  return measure(token) + inspect(token) + token.width() == diag.text.size() + 14
             ? 0
             : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "an ownerless value keeps storage, arguments and method calls");
  if (!result.canGenerateCode())
    return;
  expect(publications(result.mir).empty(),
         "ownerless constructions publish no dependency");
}

void testConfinement() {
  struct Case {
    const char *body;
    const char *code;
    const char *description;
  };
  const Case cases[] = {
      {"Token token = Token(input.view(), 3); uint64_t n = measure(token);",
       "GTI-S2045", "a dependent value is not a by-value argument"},
      {"Token token = Token(input.view(), 3); uint64_t n = inspect(token);",
       "GTI-S2045", "a dependent value is not a reference argument"},
      {"Token token = Token(input.view(), 3); uint64_t n = token.width();",
       "GTI-S2045", "a method cannot read a dependent value's fields"},
      {"Token token = Token(input.view(), 3); kept = token.text;", "GTI-S2045",
       "a dependent field read is not ownerless text"},
      {"Token token = Token(input.view(), 3); mut Token other = "
       "Token(\"x\", 1); other = token;",
       "GTI-S2045", "a dependent value cannot replace existing storage"},
      {"Token token = Token(input.view(), 3); Token& alias = token; "
       "int n = alias.kind;",
       "GTI-S2045", "a reference cannot bind a dependent value"},
      {"Token token = Token(input.view(), 3); auto read = [token]() -> "
       "uint64_t { return token.text.size(); }; uint64_t n = read();",
       "GTI-S2045", "a closure cannot capture a dependent value"},
      {"Pair pair = Pair(input.view(), other.view());", "GTI-S2045",
       "a value holds dynamic text from one owner"},
      {"Logged logged = Logged(input.view());", "GTI-S2045",
       "a class with a destructor keeps ownerless text fields"},
      {"Token token = Token(std::string(\"x\").view(), 3);", "GTI-S2045",
       "a temporary owner cannot outlive its view"},
      {"Token token = Token(owner.view(), 3); owner.append(\"!\"); "
       "int n = token.kind;",
       "GTI-S2017", "the owner stays borrowed while the value is live"},
      {"Token token = Token(owner.view(), 3); std::string_view head = "
       "token.text; owner.append(\"!\"); uint64_t n = head.size();",
       "GTI-S2017", "a view read from the value keeps the owner borrowed"},
  };
  for (const auto &entry : cases) {
    const auto result = analyzeText(
        kDeclarations +
        "int run() { std::string input{\"token text\"}; std::string "
        "other{\"other\"}; mut std::string owner{\"owner\"}; " +
        entry.body + " return 0; }\nint main() { return run(); }\n");
    expect(!result.canGenerateCode(), entry.description);
    expect(hasDiagnosticCode(result.diagnostics, entry.code),
           std::string(entry.description) + " (" + entry.code + ")");
    expect(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           std::string(entry.description) + " is diagnosed at source");
  }
  const auto escaped = analyzeText(kDeclarations + R"gti(
Token make(std::string& input) { return Token(input.view(), 3); }
int main() { return 0; }
)gti");
  expect(hasDiagnosticCode(escaped.diagnostics, "GTI-S2045"),
         "a dependent value is not returned in this phase");
}

constexpr NamedTest kTests[] = {
    {"testDependentPublication", testDependentPublication},
    {"testOwnerlessConstructionUnchanged", testOwnerlessConstructionUnchanged},
    {"testConfinement", testConfinement},
};
} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, kTests, "view fields");
}
