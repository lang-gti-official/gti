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
mut std::string_view kept = "";
class Token {
public:
  std::string_view text;
  int kind;
  Token(std::string_view text, int kind) : text(text), kind(kind) {}
  uint64_t width() { return this.text.size(); }
  void save() { kept = this.text; }
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
uint64_t measure(Token token) { return token.text.size(); }
uint64_t inspect(Token& token) { return token.text.size(); }
void keep(Token token) { kept = token.text; }
void keepRef(Token& token) { kept = token.text; }
void relayKeep(Token token) { keep(token); }
std::string_view first(Token token) { return token.text; }
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
      {"Token token = Token(input.view(), 3); keep(token);", "GTI-S2045",
       "a by-value relay cannot reach a function that stores the field"},
      {"Token token = Token(input.view(), 3); keepRef(token);", "GTI-S2045",
       "a reference relay cannot reach a function that stores the field"},
      {"Token token = Token(input.view(), 3); relayKeep(token);", "GTI-S2045",
       "a requirement forwards through a relaying function"},
      {"Token token = Token(input.view(), 3); token.save();", "GTI-S2045",
       "a receiver relay cannot reach a method that stores the field"},
      {"Token token = Token(input.view(), 3); uint64_t n = "
       "first(token).size();",
       "GTI-S2045", "a stored-view return is ownerless text"},
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

void testRelays() {
  const auto result = analyzeText(kDeclarations + R"gti(
int main() {
  std::string input{"token text"};
  Token token = Token(input.view(), 3);
  Token literal = Token("literal", 1);
  keep(literal);
  relayKeep(literal);
  return measure(token) + inspect(token) + token.width() ==
                 30 + first(literal).size() - 7
             ? 0
             : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "a dependent value relays into functions and methods that keep its "
         "text within the call");
  if (!result.canGenerateCode())
    return;
  // Field contracts: storing functions require the field; readers do not.
  std::size_t requiring = 0;
  std::size_t free = 0;
  const lang::MirFunctionInstance *keep = nullptr;
  for (const auto &function : result.mir.functionInstances()) {
    if (function.parameterTypes.size() != 1 ||
        function.parameterTypes.front().kind == lang::SemanticType::Unknown)
      continue;
    if (!function.ownerlessTextFields.empty()) {
      ++requiring;
      if (function.parameterTypes.front().kind == lang::SemanticType::Class &&
          function.returnType.kind == lang::SemanticType::Void &&
          keep == nullptr)
        keep = &function;
    } else {
      ++free;
    }
  }
  expect(requiring >= 4,
         "storing and relaying functions carry field contracts");
  expect(free >= 2, "reading functions carry no field contract");
  if (keep != nullptr) {
    auto changed = result.mir;
    const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(keep->id))
        ->ownerlessTextFields.clear();
    expect(!lang::verifyMirProgram(changed).valid(),
           "an omitted field requirement is rejected by MIR");
  }
  // A forged requirement on a reader rejects the dependent relay into it:
  // MIR checks each relay against the callee's contract itself.
  const auto published = publications(result.mir);
  if (published.empty())
    return;
  const auto token = instructionAt(const_cast<lang::MirProgram &>(result.mir),
                                   published.front())
                         .info.type;
  std::size_t forged = 0;
  for (const auto &function : result.mir.functionInstances()) {
    if (!function.ownerlessTextFields.empty() ||
        function.parameterTypes.size() != 1 ||
        function.parameterTypes.front() != token)
      continue;
    lang::SymbolId text = 0;
    for (const auto &instance : result.mir.classInstances())
      if (instance.type == function.parameterTypes.front())
        for (const auto &field : instance.declaredFields)
          if (field.type.kind == lang::SemanticType::StringView && text == 0)
            text = field.symbol;
    if (text == 0)
      continue;
    auto changed = result.mir;
    const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(function.id))
        ->ownerlessTextFields = {{.parameter = 0, .field = text}};
    ++forged;
    expect(!lang::verifyMirProgram(changed).valid(),
           "a relay into a function requiring the field is rejected by MIR");
  }
  expect(forged >= 1, "a reading relay target exists to forge");
}

void testConstructorReads() {
  // A constructor body could read a published field back through `this` as
  // ownerless text, so a constructor with a body keeps the ownerless rule.
  // Initializers and field defaults cannot reference members yet.
  const std::string declarations = R"gti(
#include <std/string>
mut std::string_view kept = "";
class Logging {
public:
  std::string_view text;
  Logging(std::string_view text) : text(text) { kept = this.text; }
};
mut int built = 0;
class Counting {
public:
  std::string_view text;
  Counting(std::string_view text) : text(text) { built = built + 1; }
};
class Plain {
public:
  std::string_view text;
  int kind;
  Plain(std::string_view input, int kind) : text(input), kind(kind) {}
};
)gti";
  for (const char *construction :
       {"Logging value = Logging(owner.view());",
        "Counting value = Counting(owner.view());"}) {
    const auto result = analyzeText(
        declarations + "int main() { std::string owner{\"owned\"}; " +
        construction + " return 0; }\n");
    expect(!result.canGenerateCode() &&
               hasDiagnosticCode(result.diagnostics, "GTI-S2045") &&
               hasDiagnosticHint(result.diagnostics,
                                 "reads the object, so it keeps its text "
                                 "fields ownerless"),
           std::string("a constructor that reads the object keeps the "
                       "ownerless rule: ") +
               construction);
    expect(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           std::string("a reading constructor is diagnosed at source: ") +
               construction);
  }
  const auto plain = analyzeText(declarations + R"gti(
int main() {
  std::string owner{"owned"};
  Plain value = Plain(owner.view(), 2);
  Logging literal = Logging("literal");
  return value.text.size() == 5 && kept.size() == 7 ? 0 : 1;
}
)gti");
  if (!plain.canGenerateCode())
    printDiagnostics(plain);
  expect(plain.canGenerateCode(),
         "an empty-body constructor publishes, and an ownerless argument "
         "still reaches a reading constructor");
}

constexpr NamedTest kTests[] = {
    {"testDependentPublication", testDependentPublication},
    {"testOwnerlessConstructionUnchanged", testOwnerlessConstructionUnchanged},
    {"testConfinement", testConfinement},
    {"testRelays", testRelays},
    {"testConstructorReads", testConstructorReads},
};
} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, kTests, "view fields");
}
