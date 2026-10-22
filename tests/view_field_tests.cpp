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
  std::string_view name() { return this.text; }
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
std::string_view second(Token token) { return first(token); }
void keepFirst(Token token) { kept = first(token); }
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
      {"Token token = Token(input.view(), 3); kept = first(token);",
       "GTI-S2045", "a returned field view keeps its owner dependency"},
      {"Token token = Token(input.view(), 3); keepFirst(token);", "GTI-S2045",
       "storing a returned field view requires that field ownerless"},
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
  return measure(token) + inspect(token) + token.width() +
                     first(token).size() + second(token).size() ==
                 50 + first(literal).size() - 7
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

// A function or method that returns one text field of a formal or the
// receiver relays that field's dependency to its result (L-OWN-02).
void testFieldResults() {
  const auto result = analyzeText(kDeclarations + R"gti(
int main() {
  std::string input{"token text"};
  Token token = Token(input.view(), 3);
  std::string_view a = first(token);
  std::string_view b = second(token);
  std::string_view c = token.name();
  Token literal = Token("literal", 1);
  kept = first(literal);
  return a.size() + b.size() + c.size() == 30 && kept.size() == 7 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "a returned field view depends on the value it reads; an ownerless "
         "value's field view is ownerless");
  if (!result.canGenerateCode())
    return;
  // Each relay names one field of argument 0 or the receiver and requires
  // none: the field's text reaches the caller's result, never storage.
  std::vector<const lang::MirFunctionInstance *> relays;
  for (const auto &function : result.mir.functionInstances())
    if (function.returnType.kind == lang::SemanticType::StringView &&
        function.returnBorrowFields.size() == 1 &&
        function.returnBorrowFields.front().sourceFields.size() == 1) {
      relays.push_back(&function);
      expect(function.ownerlessTextFields.empty(),
             "a field relay requires no ownerless field");
    }
  expect(relays.size() >= 3,
         "free, composed and method field relays carry a field result");
  const auto argumentRelay =
      std::find_if(relays.begin(), relays.end(), [](const auto *relay) {
        return relay->returnBorrowOrigin == lang::BorrowOriginKind::Argument &&
               !relay->parameterTypes.empty();
      });
  lang::SymbolId kind = 0;
  if (argumentRelay != relays.end())
    for (const auto &instance : result.mir.classInstances())
      for (const auto &field : instance.declaredFields)
        if (field.type.kind == lang::SemanticType::Int32 && kind == 0 &&
            instance.type == (*argumentRelay)->parameterTypes.front())
          kind = field.symbol;
  expect(kind != 0, "the relayed class has a non-text field to misname");
  for (const auto *relay : relays) {
    if (relay->returnBorrowOrigin != lang::BorrowOriginKind::Argument)
      continue;
    // MIR derives the returned field itself, so a wrong one is rejected.
    auto wrongField = result.mir;
    const_cast<lang::MirFunctionInstance *>(
        wrongField.findFunctionInstance(relay->id))
        ->returnBorrowFields.front()
        .sourceFields = {kind};
    expect(!lang::verifyMirProgram(wrongField).valid(),
           "a field relay naming another field is rejected by MIR");
    // Without the field, the summary would borrow the by-value formal.
    auto dropped = result.mir;
    const_cast<lang::MirFunctionInstance *>(
        dropped.findFunctionInstance(relay->id))
        ->returnBorrowFields.clear();
    expect(!lang::verifyMirProgram(dropped).valid(),
           "a field relay without its field is rejected by MIR");
  }
  // The dependent call result carries a loan on the token's owner.
  std::size_t removed = 0;
  for (const auto &function : result.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.loan && instruction.functionTarget &&
            std::any_of(relays.begin(), relays.end(), [&](const auto *relay) {
              return relay->id == *instruction.functionTarget;
            })) {
          auto changed = result.mir;
          for (auto &target : changed.functionInstances())
            if (target.id == function.id)
              for (auto &candidate :
                   const_cast<lang::MirFunctionInstance &>(target).body.blocks)
                for (auto &call : candidate.instructions)
                  if (call.id == instruction.id)
                    call.loan.reset();
          ++removed;
          expect(!lang::verifyMirProgram(changed).valid(),
                 "a dependent field relay result keeps its loan");
        }
  expect(removed >= 3, "dependent field relay results are exercised");
}

// A payload construction publishes a dynamic text argument's owner at that
// payload (L-OWN-02); a switch that observes the value binds a dependent
// view of that payload and ownerless views of the others.
const std::string kPayloadDeclarations = R"gti(
#include <std/string>
mut std::string_view kept = "";
enum Word { text(std::string_view value), count(int value) };
enum Two { both(std::string_view first, std::string_view second), none };
void take(Word word) {}
)gti";

void testPayloads() {
  const auto result = analyzeText(kPayloadDeclarations + R"gti(
int main() {
  std::string input{"token text"};
  Word word = Word::text(input.view());
  Word copy = word;
  Word moved = std::move(copy);
  mut uint64_t size = 0;
  switch (moved) {
  case Word::text(value): size = value.size(); break;
  case Word::count(value): break;
  }
  Two two = Two::both(input.view(), "fixed");
  switch (two) {
  case Two::both(first, second): kept = second; size = size + first.size(); break;
  case Two::none: break;
  }
  Word literal = Word::text("literal");
  switch (literal) {
  case Word::text(value): kept = value; break;
  case Word::count(value): break;
  }
  return size == 20 && kept.size() == 7 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "a payload holds a local owner's dynamic text, and a switch binds a "
         "dependent view of it");
  if (!result.canGenerateCode())
    return;
  std::vector<Location> payloads;
  for (const auto &location : publications(result.mir)) {
    const auto &instruction =
        instructionAt(const_cast<lang::MirProgram &>(result.mir), location);
    if (instruction.kind == lang::MirInstructionKind::Compute &&
        instruction.operation == lang::MirOperation::PayloadConstruct)
      payloads.push_back(location);
  }
  expect(payloads.size() == 2, "both dependent payload constructions publish");
  if (payloads.size() != 2)
    return;
  const auto loanOf = [](lang::MirProgram &program,
                         const Location &location) -> lang::MirLoan * {
    auto *function = const_cast<lang::MirFunctionInstance *>(
        program.findFunctionInstance(location.function));
    const auto &instruction = instructionAt(program, location);
    return instruction.loan && *instruction.loan <= function->body.loans.size()
               ? &function->body.loans[*instruction.loan - 1]
               : nullptr;
  };
  for (const auto &location : payloads) {
    const auto *loan =
        loanOf(const_cast<lang::MirProgram &>(result.mir), location);
    expect(loan != nullptr && loan->kind == lang::MirLoanKind::Stored &&
               loan->access == lang::AccessMode::ReadOnly &&
               loan->designatedFields.size() == 1 && loan->conditions.empty() &&
               !loan->carriers.empty(),
           "a published payload's loan designates one payload, "
           "unconditionally, and its bindings carry it");
  }
  // Without its publication the dynamic argument must be ownerless.
  auto unpublished = result.mir;
  instructionAt(unpublished, payloads.front()).dependentTextPublication = false;
  expect(!lang::verifyMirProgram(unpublished).valid(),
         "a payload argument without its publication is rejected by MIR");
  // A dependent value's alternative is fixed, so the loan is unconditional.
  auto conditioned = result.mir;
  {
    const auto &instruction = instructionAt(conditioned, payloads.front());
    loanOf(conditioned, payloads.front())->conditions = {
        {.enumeration = instruction.info.type,
         .variant = instruction.enumVariant.value_or(0)}};
  }
  expect(!lang::verifyMirProgram(conditioned).valid(),
         "a published payload's loan cannot be conditional");
  // Designating the other payload makes `kept = second` store dynamic text,
  // which MIR rejects by itself.
  auto retargeted = result.mir;
  {
    const auto &instruction = instructionAt(retargeted, payloads.back());
    const auto *enumeration = retargeted.findPayloadEnum(instruction.info.type);
    if (enumeration != nullptr && instruction.enumVariant &&
        *instruction.enumVariant < enumeration->variants.size() &&
        enumeration->variants[*instruction.enumVariant].payloadSymbols.size() ==
            2)
      loanOf(retargeted, payloads.back())->designatedFields = {
          enumeration->variants[*instruction.enumVariant].payloadSymbols[1]};
  }
  expect(!lang::verifyMirProgram(retargeted).valid(),
         "a payload relabeled as dependent is rejected where it is stored");

  struct Case {
    const char *body;
    const char *code;
    const char *description;
  };
  const Case cases[] = {
      {"Word word = Word::text(input.view()); switch (word) { case "
       "Word::text(value): kept = value; break; case Word::count(value): "
       "break; }",
       "GTI-S2045", "a dependent payload binding is not ownerless text"},
      {"Word word = Word::text(input.view()); switch (word) { case "
       "Word::text(auto& value): break; case Word::count(value): break; }",
       "GTI-S2045", "a reference cannot yet bind a dependent payload"},
      {"Word word = Word::text(input.view()); take(word);", "GTI-S2045",
       "a dependent payload value cannot be passed"},
      {"Word word = Word::text(std::string(\"x\").view());", "GTI-S2045",
       "a temporary owner cannot outlive its payload"},
      {"Two two = Two::both(input.view(), other.view());", "GTI-S2045",
       "a payload value holds dynamic text from one owner"},
      {"Word word = Word::text(input.view()); switch (std::move(word)) { case "
       "Word::text(value): break; case Word::count(value): break; }",
       "GTI-S2045", "a dependent payload value cannot yet be consumed"},
      {"Word word = Word::text(owner.view()); owner.append(\"!\"); switch "
       "(word) { case Word::text(value): break; case Word::count(value): "
       "break; }",
       "GTI-S2017", "the owner stays borrowed while the payload value is live"},
  };
  for (const auto &entry : cases) {
    const auto rejected = analyzeText(
        kPayloadDeclarations +
        "int run() { std::string input{\"token text\"}; std::string "
        "other{\"other\"}; mut std::string owner{\"owner\"}; " +
        entry.body + " return 0; }\nint main() { return run(); }\n");
    expect(!rejected.canGenerateCode(), entry.description);
    expect(hasDiagnosticCode(rejected.diagnostics, entry.code),
           std::string(entry.description) + " (" + entry.code + ")");
    expect(std::none_of(rejected.diagnostics.begin(),
                        rejected.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           std::string(entry.description) + " is diagnosed at source");
  }
  const auto returned = analyzeText(kPayloadDeclarations + R"gti(
Word make(std::string& input) { return Word::text(input.view()); }
int main() { return 0; }
)gti");
  expect(hasDiagnosticCode(returned.diagnostics, "GTI-S2045"),
         "a dependent payload value is not returned in this phase");
}

// A class temporary may be read within its expression (L-OWN-02). A
// dependent temporary's text field, a relay of the temporary and its method
// relay hold the owner's text; an ownerless temporary's text field is
// ownerless text that may be stored or returned.
void testTemporaries() {
  const auto result = analyzeText(kDeclarations + R"gti(
std::string_view literalText() { return Token("literal", 1).text; }
int main() {
  std::string input{"token text"};
  std::string_view field = Token(input.view(), 1).text;
  std::string_view relayed = first(Token(input.view(), 2));
  std::string_view named = Token(input.view(), 3).name();
  uint64_t size = Token(input.view(), 4).text.size() +
                  Token(input.view(), 5).width();
  kept = Pair(input.view(), "fixed").second;
  std::string_view literal = Token("literal", 6).text;
  return field.size() + relayed.size() + named.size() + size == 50 &&
                 kept.size() + literal.size() + literalText().size() == 19
             ? 0
             : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "a temporary's text field, relay and method relay hold its owner's "
         "text; an ownerless temporary's field is ownerless");
  if (!result.canGenerateCode())
    return;
  const auto found = publications(result.mir);
  expect(found.size() == 6, "each dependent temporary publishes its owner");
  if (found.size() != 6)
    return;
  auto *main = const_cast<lang::MirFunctionInstance *>(
      result.mir.findFunctionInstance(found.front().function));
  const auto loanOf = [](lang::MirProgram &program,
                         const Location &location) -> lang::MirLoan * {
    auto *function = const_cast<lang::MirFunctionInstance *>(
        program.findFunctionInstance(location.function));
    const auto &instruction = instructionAt(program, location);
    return instruction.loan && *instruction.loan <= function->body.loans.size()
               ? &function->body.loans[*instruction.loan - 1]
               : nullptr;
  };
  // Only the field read's binding carries a temporary's own loan; the others
  // end with their expression.
  std::size_t carried = 0;
  for (const auto &location : found) {
    const auto *loan =
        loanOf(const_cast<lang::MirProgram &>(result.mir), location);
    expect(location.function == main->id && loan != nullptr &&
               loan->kind == lang::MirLoanKind::Stored &&
               loan->access == lang::AccessMode::ReadOnly &&
               loan->designatedFields.size() == 1 && loan->conditions.empty(),
           "a temporary's loan designates its one published text field");
    if (loan != nullptr && !loan->carriers.empty())
      ++carried;
  }
  expect(carried == 1, "only the field read's binding carries a temporary's "
                       "own loan");
  // The relay and the method relay publish their results under loans that
  // their bindings carry.
  std::vector<Location> relays;
  for (std::size_t block = 0; block < main->body.blocks.size(); ++block)
    for (std::size_t index = 0;
         index < main->body.blocks[block].instructions.size(); ++index) {
      const auto &call = main->body.blocks[block].instructions[index];
      const auto *target =
          call.kind == lang::MirInstructionKind::Call && call.functionTarget
              ? result.mir.findFunctionInstance(*call.functionTarget)
              : nullptr;
      if (target != nullptr &&
          target->returnType.kind == lang::SemanticType::StringView &&
          target->returnBorrowFields.size() == 1)
        relays.push_back({main->id, block, index});
    }
  expect(relays.size() == 2, "a relay and a method relay of a temporary");
  for (const auto &location : relays) {
    const auto *loan =
        loanOf(const_cast<lang::MirProgram &>(result.mir), location);
    expect(loan != nullptr && loan->kind == lang::MirLoanKind::Stored &&
               !loan->carriers.empty(),
           "a temporary's relayed field result carries its owner's loan");
  }
  // Without its publication the field read's argument must be ownerless.
  const auto fieldRead =
      std::find_if(found.begin(), found.end(), [&](const auto &location) {
        const auto *loan =
            loanOf(const_cast<lang::MirProgram &>(result.mir), location);
        return loan != nullptr && !loan->carriers.empty();
      });
  if (fieldRead != found.end()) {
    auto unpublished = result.mir;
    instructionAt(unpublished, *fieldRead).dependentTextPublication = false;
    expect(!lang::verifyMirProgram(unpublished).valid(),
           "a temporary without its publication is rejected by MIR");
  }
  // The relay's result holds the owner's text, so it keeps its loan.
  if (!relays.empty()) {
    auto unloaned = result.mir;
    instructionAt(unloaned, relays.front()).loan.reset();
    expect(!lang::verifyMirProgram(unloaned).valid(),
           "a relay of a dependent temporary keeps its result loan");
  }
  // Designating the pair's other field makes `kept = ....second` store the
  // owner's text, which MIR rejects by itself.
  auto retargeted = result.mir;
  for (const auto &location : found) {
    const auto &instruction = instructionAt(retargeted, location);
    for (const auto &instance : retargeted.classInstances())
      if (instance.type == instruction.info.type &&
          instance.declaredFields.size() == 2 &&
          instance.declaredFields[1].type.kind ==
              lang::SemanticType::StringView)
        loanOf(retargeted, location)->designatedFields = {
            instance.declaredFields[1].symbol};
  }
  const auto rejected = lang::verifyMirProgram(retargeted);
  expect(!rejected.valid() &&
             std::any_of(rejected.errors.begin(), rejected.errors.end(),
                         [](const auto &error) {
                           return error.message.find("ownerless text") !=
                                  std::string::npos;
                         }),
         "a temporary's field relabeled as dependent is rejected where it is "
         "stored");

  struct Case {
    const char *body;
    const char *code;
    const char *description;
  };
  const Case cases[] = {
      {"kept = Token(input.view(), 1).text;", "GTI-S2045",
       "a dependent temporary's field is not ownerless text"},
      {"kept = first(Token(input.view(), 1));", "GTI-S2045",
       "a relay of a dependent temporary is not ownerless text"},
      {"kept = Token(input.view(), 1).name();", "GTI-S2045",
       "a method relay of a dependent temporary is not ownerless text"},
      {"Token(input.view(), 1).save();", "GTI-S2045",
       "a dependent temporary cannot reach a storing method"},
      {"keepFirst(Token(input.view(), 1));", "GTI-S2045",
       "a dependent temporary cannot reach a storing function"},
      {"uint64_t n = Logged(input.view()).text.size();", "GTI-S2045",
       "a class with a destructor keeps its text fields ownerless"},
      {"std::string_view v = Token(std::string(\"x\").view(), 1).text;",
       "GTI-S2045", "a temporary owner cannot outlive the field read"},
      {"mut std::string_view v = \"x\"; v = Token(input.view(), 1).text;",
       "GTI-S2045",
       "a temporary's field cannot be assigned to existing "
       "storage"},
      {"std::string_view v = first(Token(owner.view(), 1)); "
       "owner.append(\"!\"); uint64_t n = v.size();",
       "GTI-S2017", "the owner stays borrowed while a relayed view is live"},
  };
  for (const auto &entry : cases) {
    const auto rejectedCase = analyzeText(
        kDeclarations +
        "int run() { std::string input{\"token text\"}; mut std::string "
        "owner{\"owner\"}; " +
        entry.body + " return 0; }\nint main() { return run(); }\n");
    expect(!rejectedCase.canGenerateCode(), entry.description);
    expect(hasDiagnosticCode(rejectedCase.diagnostics, entry.code),
           std::string(entry.description) + " (" + entry.code + ")");
    expect(std::none_of(rejectedCase.diagnostics.begin(),
                        rejectedCase.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           std::string(entry.description) + " is diagnosed at source");
  }
  const auto returned = analyzeText(kDeclarations + R"gti(
std::string_view leak(std::string& input) { return Token(input.view(), 1).text; }
int main() { return 0; }
)gti");
  expect(hasDiagnosticCode(returned.diagnostics, "GTI-S2045"),
         "a dependent temporary's field is not returned in this phase");
}

// Recursive calls relay what the recursion's other returns relay: a text
// formal, a formal's text field or the receiver's.
const std::string kRecursiveDeclarations = R"gti(
#include <std/string>
mut std::string_view kept = "";
class Token {
public:
  std::string_view text;
  int kind;
  Token(std::string_view text, int kind) : text(text), kind(kind) {}
  std::string_view skip(int n) {
    if (n == 0) {
      return this.text;
    }
    return this.skip(n - 1);
  }
};
std::string_view first(std::string_view s, int n) {
  if (n == 0) {
    return s;
  }
  return first(s, n - 1);
}
std::string_view even(std::string_view s, int n) {
  if (n == 0) {
    return s;
  }
  return odd(s, n - 1);
}
std::string_view odd(std::string_view s, int n) { return even(s, n); }
T relay<T>(T s, int n) {
  if (n == 0) {
    return s;
  }
  return relay(s, n - 1);
}
std::string_view textOf(Token token, int n) {
  if (n == 0) {
    return token.text;
  }
  return textOf(token, n - 1);
}
std::string_view outer(std::string_view s, int n) { return first(s, n - 1); }
std::string_view outerText(Token token, int n) {
  return textOf(token, n - 1);
}
)gti";

// Whether a body calls the function instance `target`.
bool calls(const lang::MirBody &body, lang::HirFunctionInstanceId target) {
  return std::any_of(
      body.blocks.begin(), body.blocks.end(), [&](const auto &block) {
        return std::any_of(block.instructions.begin(), block.instructions.end(),
                           [&](const auto &instruction) {
                             return instruction.kind ==
                                        lang::MirInstructionKind::Call &&
                                    instruction.functionTarget == target;
                           });
      });
}

void testRecursiveRelays() {
  const auto result = analyzeText(kRecursiveDeclarations + R"gti(
int main() {
  std::string input{"token text"};
  Token token = Token(input.view(), 3);
  std::string_view a = first(input.view(), 3);
  std::string_view b = even(input.view(), 3);
  std::string_view c = relay(input.view(), 2);
  std::string_view d = textOf(token, 2);
  std::string_view e = token.skip(2);
  std::string_view f = outer(input.view(), 2);
  std::string_view g = outerText(token, 2);
  kept = first("literal", 2);
  return a.size() + b.size() + c.size() + d.size() + e.size() + f.size() +
                     g.size() ==
                 70 && kept.size() == 7
             ? 0
             : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "recursive and mutually recursive functions relay a dynamic text "
         "formal, a formal's text field and the receiver's");
  if (!result.canGenerateCode())
    return;
  // Each self-recursive relay names one origin and requires no ownerless
  // text: some depth returns the formal or field, and every other depth
  // relays the same one.
  std::size_t direct = 0;
  std::size_t fields = 0;
  const lang::MirFunctionInstance *relayFunction = nullptr;
  for (const auto &function : result.mir.functionInstances()) {
    if (function.returnType.kind != lang::SemanticType::StringView ||
        !calls(function.body, function.id))
      continue;
    expect(function.returnBorrowOrigin != lang::BorrowOriginKind::None &&
               function.returnBorrowParameter == 0 &&
               function.ownerlessTextParameters.empty() &&
               function.ownerlessTextFields.empty(),
           "a recursive relay names one origin and requires no ownerless text");
    if (function.returnBorrowFields.empty()) {
      ++direct;
      if (relayFunction == nullptr)
        relayFunction = &function;
    } else {
      ++fields;
    }
  }
  expect(direct == 2, "a text formal relays through `first` and `relay`");
  expect(fields == 2,
         "a field relays through `textOf` and the receiver's through `skip`");
  if (relayFunction == nullptr)
    return;
  // MIR verifies the relay independently: the recursive call keeps its
  // result loan, and the function keeps its argument origin.
  std::size_t removed = 0;
  for (std::size_t block = 0; block < relayFunction->body.blocks.size();
       ++block)
    for (std::size_t index = 0;
         index < relayFunction->body.blocks[block].instructions.size();
         ++index) {
      const auto &instruction =
          relayFunction->body.blocks[block].instructions[index];
      if (instruction.kind != lang::MirInstructionKind::Call ||
          instruction.functionTarget != relayFunction->id || !instruction.loan)
        continue;
      auto changed = result.mir;
      instructionAt(changed, {relayFunction->id, block, index}).loan.reset();
      ++removed;
      expect(!lang::verifyMirProgram(changed).valid(),
             "a recursive relay call keeps its result loan");
    }
  expect(removed == 1, "the recursive relay call carries a result loan");
  auto unrelayed = result.mir;
  const_cast<lang::MirFunctionInstance *>(
      unrelayed.findFunctionInstance(relayFunction->id))
      ->returnBorrowOrigin = lang::BorrowOriginKind::None;
  expect(!lang::verifyMirProgram(unrelayed).valid(),
         "a recursion returning its dynamic formal needs its relay contract");

  struct Case {
    const char *body;
    const char *code;
    const char *description;
  };
  const Case cases[] = {
      {"kept = first(owner.view(), 2);", "GTI-S2045",
       "a recursive relay's result keeps its owner dependency"},
      {"std::string_view v = first(owner.view(), 3); owner.append(\"!\"); "
       "uint64_t n = v.size();",
       "GTI-S2017", "a recursive relay's result keeps its owner borrowed"},
      {"std::string_view v = even(owner.view(), 3); owner.append(\"!\"); "
       "uint64_t n = v.size();",
       "GTI-S2017", "a mutual recursion's result keeps its owner borrowed"},
      {"Token token = Token(owner.view(), 1); std::string_view v = "
       "textOf(token, 2); owner.append(\"!\"); uint64_t n = v.size();",
       "GTI-S2017", "a recursive field relay keeps the field's owner borrowed"},
      {"Token token = Token(owner.view(), 1); std::string_view v = "
       "token.skip(2); owner.append(\"!\"); uint64_t n = v.size();",
       "GTI-S2017",
       "a recursive receiver relay keeps the field's owner borrowed"},
  };
  for (const auto &entry : cases) {
    const auto rejected = analyzeText(
        kRecursiveDeclarations +
        "int run() { mut std::string owner{\"owner\"}; " + entry.body +
        " return 0; }\nint main() { return run(); }\n");
    expect(!rejected.canGenerateCode(), entry.description);
    expect(hasDiagnosticCode(rejected.diagnostics, entry.code),
           std::string(entry.description) + " (" + entry.code + ")");
    expect(std::none_of(rejected.diagnostics.begin(),
                        rejected.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           std::string(entry.description) + " is diagnosed at source");
  }
  // A recursion that permutes its formals may return either one, so it
  // relays neither: a text formal is rejected at its return, and a field
  // requires ownerless text in both formals.
  for (const char *source : {R"gti(
std::string_view pick(std::string_view s, std::string_view t, int n) {
  if (n == 0) {
    return s;
  }
  return pick(t, s, n - 1);
}
int main() { return pick("ab", "xyz", 1).size() == 3 ? 0 : 1; }
)gti",
                             R"gti(
#include <std/string>
struct Pair {
  std::string_view text;
  int line;
};
std::string_view pick(Pair a, Pair b, int n) {
  if (n == 0) {
    return a.text;
  }
  return pick(b, a, n - 1);
}
int main() {
  std::string owner{"dynamic"};
  Pair a{.text = "ab", .line = 1};
  Pair b{.text = owner.view(), .line = 2};
  return pick(a, b, 1).size() == 7 ? 0 : 1;
}
)gti",
                             R"gti(
#include <std/string>
std::string_view first(std::string_view s, int n) {
  if (n == 0) {
    return s;
  }
  return first(s, n - 1);
}
std::string_view leak() {
  std::string local{"local"};
  return first(local.view(), 2);
}
int main() { return leak().size() == 5 ? 0 : 1; }
)gti"}) {
    const auto rejected = analyzeText(source);
    expect(!rejected.canGenerateCode() &&
               hasDiagnosticCode(rejected.diagnostics, "GTI-S2045"),
           "a permuting recursion relays neither formal, and a relay cannot "
           "return a local owner's view");
    expect(std::none_of(rejected.diagnostics.begin(),
                        rejected.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           "recursive relay rejections are diagnosed at source");
  }
}

constexpr NamedTest kTests[] = {
    {"testDependentPublication", testDependentPublication},
    {"testOwnerlessConstructionUnchanged", testOwnerlessConstructionUnchanged},
    {"testConfinement", testConfinement},
    {"testRelays", testRelays},
    {"testConstructorReads", testConstructorReads},
    {"testFieldResults", testFieldResults},
    {"testPayloads", testPayloads},
    {"testTemporaries", testTemporaries},
    {"testRecursiveRelays", testRecursiveRelays},
};
} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, kTests, "view fields");
}
