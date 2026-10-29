#include "compiler_test_fixture.h"
#include "gti/hir.h"
#include "gti/mir_verifier.h"

#include <set>

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
import std.string;
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
  // A value over a reference formal's referent is returned (phase 2c); one
  // over a local owner is not.
  const auto returned = analyzeText(kDeclarations + R"gti(
Token make(std::string& input) { return Token(input.view(), 3); }
int main() {
  std::string input{"made"};
  Token token = make(input);
  return token.width() == 4 ? 0 : 1;
}
)gti");
  if (!returned.canGenerateCode())
    printDiagnostics(returned);
  expect(returned.canGenerateCode() &&
             lang::verifyMirProgram(returned.mir).valid(),
         "a dependent value over a reference formal's referent is returned");
  const auto escaped = analyzeText(kDeclarations + R"gti(
Token make() { std::string input{"local"}; return Token(input.view(), 3); }
int main() { return 0; }
)gti");
  expect(hasDiagnosticCode(escaped.diagnostics, "GTI-S2045"),
         "a dependent value over a local owner is not returned");
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
import std.string;
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
import std.string;
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
Word make() { std::string input{"local"}; return Word::text(input.view()); }
int main() { return 0; }
)gti");
  expect(hasDiagnosticCode(returned.diagnostics, "GTI-S2045"),
         "a dependent payload value over a local owner is not returned");
}

// A switch's by-value binding of a dependent payload carries the subject's
// loan (L-OWN-02), so a local copied from it, and a copy of that copy, is a
// dependent view carrying the same loan, as a copy of a published field read
// is.
void testCopiedPayloadBindings() {
  const auto result = analyzeText(kPayloadDeclarations + R"gti(
int main() {
  std::string input{"token text"};
  Word word = Word::text(input.view());
  mut uint64_t size = 0;
  switch (word) {
  case Word::text(value):
    std::string_view copied = value;
    std::string_view again = copied;
    size = copied.size() + again.size();
    break;
  case Word::count(value): break;
  }
  return size == 20 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "a copy of a dependent payload binding is a verified dependent view");
  const auto published = publications(result.mir);
  expect(published.size() == 1, "the payload construction publishes");
  if (!result.canGenerateCode() || published.size() != 1)
    return;
  const auto location = published.front();
  const auto &body = result.mir.findFunctionInstance(location.function)->body;
  const auto &publication =
      body.blocks[location.block].instructions[location.instruction];
  const auto *loan =
      publication.loan ? body.findLoan(*publication.loan) : nullptr;
  expect(loan != nullptr && loan->kind == lang::MirLoanKind::Stored,
         "the published payload has its own Stored loan");
  if (loan == nullptr)
    return;
  // The copies are the locals initialized from a load of another binding.
  std::unordered_map<lang::MirValueId, lang::HirBindingId> loaded;
  std::vector<lang::HirBindingId> copies;
  const auto bindingOf = [&](lang::MirPlaceId id) -> lang::HirBindingId {
    const auto *place = body.findPlace(id);
    return place && place->root == lang::MirPlaceRootKind::Binding &&
                   place->projections.empty()
               ? place->binding
               : 0;
  };
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Load &&
          instruction.result && instruction.operands.size() == 1 &&
          instruction.operands[0].kind == lang::MirOperandKind::Copy)
        if (const auto source = bindingOf(instruction.operands[0].place))
          loaded.emplace(*instruction.result, source);
      if (instruction.kind == lang::MirInstructionKind::Initialize &&
          instruction.destination && instruction.operands.size() == 1 &&
          instruction.operands[0].kind == lang::MirOperandKind::Value &&
          loaded.contains(instruction.operands[0].value))
        if (const auto destination = bindingOf(*instruction.destination))
          copies.push_back(destination);
    }
  expect(copies.size() == 2, "both views are copied from a binding");
  for (const auto copy : copies) {
    expect(std::find(loan->carriers.begin(), loan->carriers.end(), copy) !=
               loan->carriers.end(),
           "a copied view carries the payload's loan");
    // MIR derives that the copy holds the owner's text, so a copy that
    // does not carry the loan leaves that text unretained.
    auto dropped = result.mir;
    std::erase(const_cast<lang::MirFunctionInstance *>(
                   dropped.findFunctionInstance(location.function))
                   ->body.loans[loan->id - 1]
                   .carriers,
               copy);
    expect(!lang::verifyMirProgram(dropped).valid(),
           "MIR rejects a copied view that does not carry the payload's loan");
  }
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

const std::string kPublishedDeclarations = R"gti(
Token makeToken(std::string_view t) { return Token(t, 1); }
Token viaHelper(std::string_view t) { return makeToken(t); }
Pair makePair(std::string_view a, std::string_view b) { return Pair(a, b); }
Token keepsLocal(std::string_view t) { Token x = makeToken(t); return x; }
)gti";

// A function that returns a fresh value whose text field holds one of its
// text formals relays it (L-OWN-02 phase 2): each call publishes its own
// argument's owner there, as a constructor does. This covers generic
// functions after substitution, such as std::make_pair and unexpected.
void testPublishedResults() {
  const auto result = analyzeText("import std.utility;\n" + kDeclarations +
                                  kPublishedDeclarations + R"gti(
int main() {
  std::string input{"token text"};
  Token made = makeToken(input.view());
  Token composed = viaHelper(input.view());
  Pair pair = makePair(input.view(), "fixed");
  kept = pair.second;
  std::string_view field = makeToken(input.view()).text;
  std::pair<std::string_view, int> generic = std::make_pair(input.view(), 1);
  std::unexpected_value<std::string_view> failure = unexpected(input.view());
  Token literal = makeToken("literal");
  kept = literal.text;
  return made.text.size() + composed.text.size() + pair.first.size() +
                     field.size() + generic.first.size() ==
                 50 &&
             kept.size() == 7
             ? 0
             : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "a returned fresh value relays its text formal; each call publishes "
         "its argument's owner");
  if (!result.canGenerateCode())
    return;
  // The factories, the helper, the pair maker and the generic instances each
  // relay a formal; MIR derives and checks every relay.
  std::size_t relaying = 0;
  const lang::MirFunctionInstance *pairMaker = nullptr;
  for (const auto &function : result.mir.functionInstances()) {
    if (function.publishedTextFields.empty())
      continue;
    ++relaying;
    expect(function.ownerlessTextParameters.empty(),
           "a relayed formal carries no ownerless requirement");
    if (function.publishedTextFields.size() == 2)
      pairMaker = &function;
  }
  expect(relaying >= 5, "factories and generic instances relay formals");
  expect(pairMaker != nullptr, "a two-field result relays both formals");
  const auto found = publications(result.mir);
  std::vector<Location> calls;
  for (const auto &location : found) {
    const auto &instruction =
        instructionAt(const_cast<lang::MirProgram &>(result.mir), location);
    if (instruction.kind == lang::MirInstructionKind::Call)
      calls.push_back(location);
  }
  expect(calls.size() == 6, "each dynamic call publishes its argument's owner");
  const auto loanOf = [](lang::MirProgram &program,
                         const Location &location) -> lang::MirLoan * {
    auto *function = const_cast<lang::MirFunctionInstance *>(
        program.findFunctionInstance(location.function));
    const auto &instruction = instructionAt(program, location);
    return instruction.loan && *instruction.loan <= function->body.loans.size()
               ? &function->body.loans[*instruction.loan - 1]
               : nullptr;
  };
  for (const auto &location : calls) {
    const auto *loan =
        loanOf(const_cast<lang::MirProgram &>(result.mir), location);
    const auto &instruction =
        instructionAt(const_cast<lang::MirProgram &>(result.mir), location);
    expect(loan != nullptr && loan->kind == lang::MirLoanKind::Stored &&
               loan->access == lang::AccessMode::ReadOnly &&
               loan->designatedFields.size() == 1 &&
               instruction.borrowFields.size() == 1 &&
               instruction.borrowFields.front().resultFields ==
                   loan->designatedFields,
           "a publishing call's loan designates its one published field");
  }
  if (calls.empty() || pairMaker == nullptr)
    return;
  // Without its relay the callee would store the formal as ownerless text.
  auto unrelayed = result.mir;
  for (const auto &function : unrelayed.functionInstances())
    if (function.id == pairMaker->id)
      const_cast<lang::MirFunctionInstance &>(function)
          .publishedTextFields.clear();
  expect(!lang::verifyMirProgram(unrelayed).valid(),
         "a callee whose relay is omitted is rejected by MIR");
  // Without its publication the call's dynamic argument must be ownerless.
  auto unpublished = result.mir;
  instructionAt(unpublished, calls.front()).dependentTextPublication = false;
  expect(!lang::verifyMirProgram(unpublished).valid(),
         "a call without its publication is rejected by MIR");
  // Designating the pair's other field makes `kept = pair.second` store
  // the owner's text, which MIR rejects by itself.
  auto retargeted = result.mir;
  for (const auto &location : calls) {
    const auto &instruction = instructionAt(retargeted, location);
    const auto *target =
        instruction.functionTarget
            ? retargeted.findFunctionInstance(*instruction.functionTarget)
            : nullptr;
    if (target != nullptr && target->id == pairMaker->id)
      loanOf(retargeted, location)->designatedFields = {
          target->publishedTextFields.back().field};
  }
  const auto stored = lang::verifyMirProgram(retargeted);
  expect(!stored.valid() &&
             std::any_of(stored.errors.begin(), stored.errors.end(),
                         [](const auto &error) {
                           return error.message.find("ownerless text") !=
                                  std::string::npos;
                         }),
         "a published field relabeled as dependent is rejected where it is "
         "stored");
  // A call semantics marks provisional requires its text arguments to be
  // ownerless, so the helper's forwarded formal would need a requirement.
  auto provisional = result.mir;
  for (const auto &function : provisional.functionInstances())
    for (auto &block :
         const_cast<lang::MirFunctionInstance &>(function).body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Call &&
            !function.publishedTextFields.empty() && instruction.result)
          instruction.provisionalTextRelays = true;
  expect(!lang::verifyMirProgram(provisional).valid(),
         "a provisional relay that semantics did not declare is rejected");

  struct Case {
    const char *body;
    const char *code;
    const char *description;
  };
  const Case cases[] = {
      {"kept = makeToken(input.view()).text;", "GTI-S2045",
       "a published field is not ownerless text"},
      {"Token t = makeToken(input.view()); kept = t.text;", "GTI-S2045",
       "a published value's field is not ownerless text"},
      {"Token t = makeToken(std::string(\"x\").view());", "GTI-S2045",
       "a temporary owner cannot be published"},
      {"Pair p = makePair(input.view(), owner.view());", "GTI-S2045",
       "a published value holds dynamic text from one owner"},
      {"Token t = keepsLocal(input.view());", "GTI-S2045",
       "a formal published into a local requires ownerless text"},
      {"Token t = makeToken(owner.view()); owner.append(\"!\"); "
       "uint64_t n = t.text.size();",
       "GTI-S2017", "the owner stays borrowed while a published value lives"},
  };
  for (const auto &entry : cases) {
    const auto rejectedCase = analyzeText(
        kDeclarations + kPublishedDeclarations +
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
  const auto leaked = analyzeText(kDeclarations + kPublishedDeclarations +
                                  R"gti(
Token leak() { std::string input{"x"}; return makeToken(input.view()); }
int main() { return 0; }
)gti");
  expect(hasDiagnosticCode(leaked.diagnostics, "GTI-S2045"),
         "a published value of a local owner is not returned");
}

const std::string kOrderDeclarations = R"gti(
class Label<T> {
public:
  T text;
  int kind;
  Label(T text, int kind) : text(text), kind(kind) {}
};
class Point {
public:
  int x;
  int y;
  Point(int x, int y) : x(x), y(y) {}
};
Label<T> outerLabel<T>(T input) { return innerLabel(input); }
Label<T> innerLabel<T>(T input) { return Label<T>(input, 3); }
Token countdown(std::string_view text, int n) {
  if (n == 0)
    return Token(text, n);
  return countdown(text, n - 1);
}
Label<T> countdownLabel<T>(T text, int n) {
  if (n == 0)
    return Label<T>(text, n);
  return countdownLabel(text, n - 1);
}
Point walk(std::string_view text, int n) {
  if (n == 0)
    return Point(n, 2);
  std::string local{"abc"};
  return walk(local.view(), n - 1);
}
)gti";

// A generic function relays through a generic call whose callee is declared
// after it, because the open body records the composition. A recursive call
// whose result has a text field requires ownerless text; one whose result has
// none takes any text.
void testPublishedResultOrder() {
  const auto result = analyzeText(kDeclarations + kOrderDeclarations + R"gti(
int main() {
  std::string input{"ordered"};
  Label<std::string_view> made = outerLabel(input.view());
  Token counted = countdown("literal", 2);
  kept = counted.text;
  std::string_view literal = "literal";
  Label<std::string_view> label = countdownLabel(literal, 2);
  Point point = walk(input.view(), 2);
  return made.text.size() + label.text.size() == 14 && point.y == 2 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "relays compose in any declaration order; recursion without a text "
         "result takes dynamic text");
  if (!result.canGenerateCode())
    return;
  const lang::MirFunctionInstance *outer = nullptr;
  std::size_t genericRelays = 0;
  std::size_t recursiveRelays = 0;
  for (const auto &function : result.mir.functionInstances()) {
    if (function.publishedTextFields.size() != 1)
      continue;
    if (function.ownerlessTextParameters == std::vector<std::size_t>{0}) {
      // The recursive function relays its construction's formal and requires
      // the formal it passes to itself. A generic one's instance is analyzed
      // after its open body and relays both; callers are checked against the
      // open declaration's requirement.
      ++recursiveRelays;
      continue;
    }
    if (function.typeArguments.size() != 1 ||
        !function.ownerlessTextParameters.empty())
      continue;
    ++genericRelays;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.functionTarget)
          if (const auto *target =
                  result.mir.findFunctionInstance(*instruction.functionTarget);
              target != nullptr && !target->publishedTextFields.empty())
            outer = &function;
  }
  expect(genericRelays == 3, "the generic instances relay their formal");
  expect(recursiveRelays == 1,
         "a recursive call with a text result requires ownerless text");
  expect(outer != nullptr, "the wrapper relays through its callee's relay");
  const auto found = publications(result.mir);
  expect(std::count_if(found.begin(), found.end(),
                       [&](const Location &location) {
                         return instructionAt(
                                    const_cast<lang::MirProgram &>(result.mir),
                                    location)
                                    .kind == lang::MirInstructionKind::Call;
                       }) == 1,
         "only the dynamic wrapper call publishes its argument's owner");
  if (outer == nullptr)
    return;
  auto unrelayed = result.mir;
  for (const auto &function : unrelayed.functionInstances())
    if (function.id == outer->id)
      const_cast<lang::MirFunctionInstance &>(function)
          .publishedTextFields.clear();
  expect(!lang::verifyMirProgram(unrelayed).valid(),
         "a composed relay that is omitted is rejected by MIR");

  for (const char *body :
       {"Token t = countdown(input.view(), 2);",
        "Label<std::string_view> l = countdownLabel(input.view(), 2);"}) {
    const auto rejected =
        analyzeText(kDeclarations + kOrderDeclarations +
                    "int run() { std::string input{\"x\"}; " + body +
                    " return 0; }\nint main() { return run(); }\n");
    expect(hasDiagnosticCode(rejected.diagnostics, "GTI-S2045"),
           std::string("a recursive text result requires ownerless text: ") +
               body);
    expect(std::none_of(rejected.diagnostics.begin(),
                        rejected.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           std::string("diagnosed at source: ") + body);
  }
}

// Recursive calls relay what the recursion's other returns relay: a text
// formal, a formal's text field or the receiver's.
const std::string kRecursiveDeclarations = R"gti(
import std.string;
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
import std.string;
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
import std.string;
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

const std::string kGenericBox = R"gti(
class Box<T> {
public:
  T value;
  int kind;
  Box(T value, int kind) : value(value), kind(kind) {}
  int one() { return this.kind; }
  T name() { return this.value; }
  T& get() { return this.value; }
  void save(mut T& out) { out = this.value; }
};
class Cell<T> {
public:
  mut T value;
  Cell(T value) : value(value) {}
  void take(Cell<T>& other) mut { this.value = other.value; }
};
)gti";

// A method of a generic class carries its open contract to every instance.
// A read of a type-parameter field other than the method's relayed return
// requires the field ownerless, which binds only text instances; a return of
// it relays the field where the instance makes it text (L-OWN-02).
void testGenericClassContracts() {
  const auto result = analyzeText(kDeclarations + kGenericBox + R"gti(
int main() {
  std::string input{"generic"};
  Box<std::string_view> box = Box<std::string_view>(input.view(), 3);
  std::string_view named = box.name();
  std::string_view got = box.get();
  Box<int> number = Box<int>(5, 4);
  mut int out = 0;
  number.save(out);
  return uint64_t(box.one()) + named.size() + got.size() + uint64_t(out) == 22
             ? 0
             : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "a dependent generic value calls its observer and relay methods");
  if (!result.canGenerateCode())
    return;
  // The text instance's `save` requires its receiver's field; the `int`
  // instance requires nothing.
  const auto saveOf = [](const lang::MirFunctionInstance &function,
                         lang::SemanticType::Kind referent) {
    return function.owner &&
           function.returnType.kind == lang::SemanticType::Void &&
           function.parameterTypes.size() == 1 &&
           function.parameterTypes.front().kind ==
               lang::SemanticType::Reference &&
           function.parameterTypes.front().arguments.size() == 1 &&
           function.parameterTypes.front().arguments.front().kind == referent;
  };
  const lang::MirFunctionInstance *textSave = nullptr;
  std::size_t intSaves = 0;
  for (const auto &function : result.mir.functionInstances())
    if (saveOf(function, lang::SemanticType::Int32)) {
      ++intSaves;
      expect(function.ownerlessTextFields.empty(),
             "the int instance of save requires nothing");
    }
  expect(intSaves == 1, "the int instance of save is used");
  const auto stored = analyzeText(kDeclarations + kGenericBox + R"gti(
int main() {
  mut std::string_view out = "";
  Box<std::string_view> box = Box<std::string_view>("literal", 1);
  box.save(out);
  mut Cell<std::string_view> cell = Cell<std::string_view>("cell");
  Cell<std::string_view> other = Cell<std::string_view>("other");
  cell.take(other);
  return out.size() == 7 && cell.value.size() == 5 ? 0 : 1;
}
)gti");
  if (!stored.canGenerateCode())
    printDiagnostics(stored);
  expect(stored.canGenerateCode(),
         "an ownerless generic value stores its text");
  for (const auto &function : stored.mir.functionInstances())
    if (saveOf(function, lang::SemanticType::StringView) &&
        !function.ownerlessTextFields.empty()) {
      textSave = &function;
      auto changed = stored.mir;
      const_cast<lang::MirFunctionInstance *>(
          changed.findFunctionInstance(function.id))
          ->ownerlessTextFields.clear();
      expect(!lang::verifyMirProgram(changed).valid(),
             "MIR derives the text instance's field requirement");
    }
  expect(textSave != nullptr,
         "the text instance of save requires its receiver's field");

  struct Case {
    const char *body;
    const char *code;
  };
  for (const Case &entry : {
           Case{"kept = box.name();", "GTI-S2045"},
           Case{"kept = box.get();", "GTI-S2045"},
           Case{"mut std::string_view out = \"\"; box.save(out);", "GTI-S2045"},
           Case{"std::string_view named = box.name(); owner.append(\"!\"); "
                "uint64_t n = named.size();",
                "GTI-S2017"},
           Case{"mut Cell<std::string_view> cell = "
                "Cell<std::string_view>(\"cell\"); Cell<std::string_view> "
                "other = Cell<std::string_view>(owner.view()); "
                "cell.take(other);",
                "GTI-S2045"},
       }) {
    const auto rejected = analyzeText(
        kDeclarations + kGenericBox +
        "int run() { mut std::string owner{\"owner\"}; "
        "Box<std::string_view> box = Box<std::string_view>(owner.view(), 1); " +
        entry.body + " return 0; }\nint main() { return run(); }\n");
    expect(hasDiagnosticCode(rejected.diagnostics, entry.code),
           std::string("a dependent generic value is checked at source: ") +
               entry.body);
    expect(std::none_of(rejected.diagnostics.begin(),
                        rejected.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           std::string("diagnosed at source: ") + entry.body);
  }
}

// A generic class method's type-parameter formal is opaque to its open
// analysis: in an instance whose argument type has text fields, any use may
// let the whole value escape, as `std::vector<T>::push_back` stores it.
void testTypeParameterFormals() {
  const auto result = analyzeText(kDeclarations + R"gti(
import std.vector;
int main() {
  mut std::vector<Token> tokens{};
  tokens.push_back(Token("literal", 1));
  return tokens.size() == 1 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "a container stores an ownerless value with text fields");
  std::size_t whole = 0;
  for (const auto &function : result.mir.functionInstances()) {
    if (!function.owner || function.parameterTypes.size() != 1 ||
        function.parameterTypes.front().kind != lang::SemanticType::Class ||
        function.ownerlessTextFields.empty())
      continue;
    ++whole;
    auto changed = result.mir;
    const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(function.id))
        ->ownerlessTextFields.clear();
    expect(!lang::verifyMirProgram(changed).valid(),
           "MIR derives the whole use of a type-parameter formal");
  }
  expect(whole >= 1, "push_back requires its argument's text fields");
  const auto rejected = analyzeText(kDeclarations + R"gti(
import std.vector;
int run() {
  std::string owner{"owner"};
  Token token = Token(owner.view(), 1);
  mut std::vector<Token> tokens{};
  tokens.push_back(token);
  return 0;
}
int main() { return run(); }
)gti");
  expect(hasDiagnosticCode(rejected.diagnostics, "GTI-S2045") &&
             std::none_of(rejected.diagnostics.begin(),
                          rejected.diagnostics.end(),
                          [](const auto &diagnostic) {
                            return diagnostic.code.starts_with("GTI-B");
                          }),
         "a container cannot store a value over a local owner");
}

const std::string kPayloadPaths = R"gti(
import std.optional;
import std.panic;
import std.string;
mut std::string_view kept = "";
enum Word { text(std::string_view value), none };
class Line {
public:
  Word word;
  int column;
  Line(std::string_view text, int column)
      : word(Word::text(text)), column(column) {}
  bool hasText() {
    switch (this.word) {
    case Word::text: return true;
    case Word::none: return false;
    }
  }
  void save() {
    switch (this.word) {
    case Word::text(value): kept = value; break;
    case Word::none: break;
    }
  }
};
enum Slot<T> { full(T value), empty };
class Holder<T> {
public:
  Slot<T> slot;
  Holder(T value) : slot(Slot<T>::full(std::move(value))) {}
  T& get() {
    switch (this.slot) {
    case Slot<T>::full(auto& value): return value;
    case Slot<T>::empty: std::panic("empty");
    }
  }
  T& again() { return this.get(); }
};
class Token {
public:
  std::string_view text;
  int kind;
  Token(std::string_view text, int kind) : text(text), kind(kind) {}
  std::string_view& ref() { return this.text; }
  std::string_view& alias() { return this.ref(); }
};
void keepLine(Line line) {
  switch (line.word) {
  case Word::text(value): kept = value; break;
  case Word::none: break;
  }
}
void keepDeref(std::optional<std::string_view>& value) { kept = *value; }
uint64_t width(std::optional<std::string_view>& value) {
  return (*value).size();
}
)gti";

// The dependent case of each payload-path shape is diagnosed at source.
void expectPayloadRejected(const std::string &setup, const std::string &body,
                           const char *code) {
  const auto rejected = analyzeText(
      kPayloadPaths + "int run() { mut std::string owner{\"owner\"}; " + setup +
      " " + body + " return 0; }\nint main() { return run(); }\n");
  expect(hasDiagnosticCode(rejected.diagnostics, code),
         std::string("a dependent payload path is checked at source: ") + body);
  expect(std::none_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code.starts_with("GTI-B");
                      }),
         std::string("diagnosed at source: ") + body);
}

// A constructor that stores its text formal as a field's payload publishes
// the argument's owner at [field, payload] (L-OWN-02). A switch on the field
// observes the payload, a payload accessor relays the path, and a method
// returning another accessor's reference composes its relay.
void testPayloadPaths() {
  const auto result = analyzeText(kPayloadPaths + R"gti(
int main() {
  std::string input{"payload"};
  Line line = Line(input.view(), 3);
  mut uint64_t total = 0;
  switch (line.word) {
  case Word::text(value): total = total + value.size(); break;
  case Word::none: break;
  }
  if (line.hasText()) {
    total = total + 1;
  }
  Holder<std::string_view> holder = Holder<std::string_view>(input.view());
  std::string_view again = holder.again();
  Holder<int> number = Holder<int>(2);
  Token token = Token(input.view(), 1);
  total = total + holder.get().size() + again.size() + token.alias().size();
  return total + uint64_t(number.again()) == 31 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "dependent values observe and relay payload paths");
  if (!result.canGenerateCode())
    return;
  // Each constructor publication designates a path through its payload enum
  // field; MIR verifies the path against the constructor's own initializer.
  std::size_t nested = 0;
  for (const auto &location : publications(result.mir)) {
    auto changed = result.mir;
    auto &instruction = instructionAt(changed, location);
    if (instruction.kind != lang::MirInstructionKind::Construct ||
        !instruction.loan)
      continue;
    auto *function = const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(location.function));
    auto loan =
        std::find_if(function->body.loans.begin(), function->body.loans.end(),
                     [&](const auto &candidate) {
                       return candidate.id == *instruction.loan;
                     });
    if (loan == function->body.loans.end() ||
        loan->designatedFields.size() != 2)
      continue;
    ++nested;
    loan->designatedFields.pop_back();
    expect(!lang::verifyMirProgram(changed).valid(),
           "MIR rejects a payload publication that designates the field "
           "alone");
  }
  expect(nested == 2, "both constructor payload inputs publish a path");

  // A switch binding a contract formal's payload requires it ownerless; MIR
  // derives the requirement where the payload escapes.
  const auto stored = analyzeText(kPayloadPaths + R"gti(
int main() {
  Line line = Line("literal", 1);
  keepLine(line);
  line.save();
  std::optional<std::string_view> maybe = std::optional<std::string_view>("x");
  keepDeref(maybe);
  return kept.size() == 1 ? 0 : 1;
}
)gti");
  if (!stored.canGenerateCode())
    printDiagnostics(stored);
  expect(stored.canGenerateCode(), "ownerless values store their payloads");
  std::size_t payloadContracts = 0;
  for (const auto &function : stored.mir.functionInstances()) {
    if (std::none_of(function.ownerlessTextFields.begin(),
                     function.ownerlessTextFields.end(),
                     [](const auto &field) { return field.payload != 0; }))
      continue;
    ++payloadContracts;
    auto changed = stored.mir;
    const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(function.id))
        ->ownerlessTextFields.clear();
    expect(!lang::verifyMirProgram(changed).valid(),
           "MIR derives a payload requirement independently");
  }
  expect(payloadContracts >= 3,
         "keepLine, save and keepDeref require their payloads");

  const std::string line = "Line line = Line(owner.view(), 1);";
  const std::string holder = "Holder<std::string_view> holder = "
                             "Holder<std::string_view>(owner.view());";
  struct Case {
    const std::string *setup;
    const char *body;
    const char *code;
  };
  for (const Case &entry : {
           Case{&line,
                "switch (line.word) { case Word::text(value): kept = value; "
                "break; case Word::none: break; }",
                "GTI-S2045"},
           Case{&line, "Word word = line.word;", "GTI-S2045"},
           Case{&line, "keepLine(line);", "GTI-S2045"},
           Case{&line, "line.save();", "GTI-S2045"},
           Case{&line,
                "Line moved = std::move(line); switch (moved.word) { case "
                "Word::text(value): kept = value; break; case Word::none: "
                "break; }",
                "GTI-S2045"},
           Case{&line,
                "owner.append(\"!\"); switch (line.word) { case "
                "Word::text(value): break; case Word::none: break; }",
                "GTI-S2017"},
           Case{&holder, "kept = holder.get();", "GTI-S2045"},
           Case{&holder, "std::string_view view = holder.again(); kept = view;",
                "GTI-S2045"},
           Case{&holder,
                "std::string_view view = holder.again(); "
                "owner.append(\"!\"); uint64_t n = view.size();",
                "GTI-S2017"},
       })
    expectPayloadRejected(*entry.setup, entry.body, entry.code);
  expectPayloadRejected("Token token = Token(owner.view(), 1);",
                        "kept = token.alias();", "GTI-S2045");
}

// `std::optional<std::string_view>` and `expected<std::string_view, E>` are
// the same shape: a constructor payload input, a tag-only `has_value`, a
// payload accessor and operators composing it.
void testOptionalViews() {
  const auto result = analyzeText(kPayloadPaths + R"gti(
int main() {
  std::string input{"optional"};
  std::optional<std::string_view> maybe =
      std::optional<std::string_view>(input.view());
  mut uint64_t total = 0;
  if (maybe && maybe.has_value()) {
    std::string_view view = *maybe;
    total = maybe.value().size() + view.size() + width(maybe);
  }
  expected<std::string_view, int> result =
      expected<std::string_view, int>(input.view());
  if (result) {
    total = total + result.value().size();
  }
  return total == 32 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "an optional or expected over a local owner's text observes it");

  const std::string maybe = "std::optional<std::string_view> maybe = "
                            "std::optional<std::string_view>(owner.view());";
  const std::string result2 = "expected<std::string_view, int> result = "
                              "expected<std::string_view, int>(owner.view());";
  struct Case {
    const std::string *setup;
    const char *body;
    const char *code;
  };
  for (const Case &entry : {
           Case{&maybe, "kept = maybe.value();", "GTI-S2045"},
           Case{&maybe, "kept = *maybe;", "GTI-S2045"},
           Case{&maybe, "std::string_view view = *maybe; kept = view;",
                "GTI-S2045"},
           Case{&maybe,
                "std::string_view view = *maybe; owner.append(\"!\"); "
                "uint64_t n = view.size();",
                "GTI-S2017"},
           Case{&maybe,
                "std::string_view view = maybe.value(); owner.append(\"!\"); "
                "uint64_t n = view.size();",
                "GTI-S2017"},
           Case{&maybe, "uint64_t n = maybe.value_or(\"none\").size();",
                "GTI-S2045"},
           Case{&maybe, "keepDeref(maybe);", "GTI-S2045"},
           Case{&maybe,
                "mut std::optional<std::string_view> other = "
                "std::optional<std::string_view>(\"x\"); other = "
                "std::move(maybe);",
                "GTI-S2045"},
           Case{&result2, "kept = result.value();", "GTI-S2045"},
       })
    expectPayloadRejected(*entry.setup, entry.body, entry.code);
}

const std::string kConversions = R"gti(
import std.optional;
import std.string;
mut std::string_view kept = "";
class Token {
public:
  std::string_view text;
  int kind;
  Token(std::string_view text, int kind) : text(text), kind(kind) {}
  std::string_view name() { return this.text; }
};
enum Word { text(std::string_view value), none };
class Wrap {
public:
  Word word;
  Wrap(Token token) : word(Word::text(token.text)) {}
};
class Copy {
public:
  std::string_view text;
  Copy(Token token) : text(token.text) {}
};
class Named {
public:
  std::string_view text;
  Named(Token token) : text(token.name()) {}
};
class Leaky {
public:
  std::string_view text;
  Leaky(Token token) : text(token.text) { kept = token.text; }
};
class Twice {
public:
  std::string_view first;
  std::string_view second;
  Twice(Token token) : first(token.text), second(token.text) {}
};
void keepToken(Token token) { kept = token.text; }
expected<int, std::string_view> parse(std::string& input) {
  if (input.empty()) {
    return 0;
  }
  return unexpected(input.view());
}
expected<int, std::string_view> parseAgain(std::string& input) {
  return parse(input);
}
Token tokenOf(std::string& input) { return Token(input.view(), 1); }
Token viaLocal(std::string& input) {
  Token token = Token(input.view(), 2);
  return token;
}
Word wordOf(std::string& input) { return Word::text(input.view()); }
std::optional<std::string_view> find(std::string& input) {
  if (input.empty()) {
    return std::nullopt;
  }
  return input.view();
}
)gti";

// Each dependent form of a conversion or return shape is diagnosed at
// source, never by the backend.
void expectConversionRejected(const std::string &declarations,
                              const std::string &body, const char *code) {
  const auto rejected =
      analyzeText(kConversions + declarations +
                  "int run() { mut std::string owner{\"owner\"}; " + body +
                  " return 0; }\nint main() { return run(); }\n");
  expect(hasDiagnosticCode(rejected.diagnostics, code),
         std::string("a dependent conversion is checked at source: ") +
             declarations + body);
  expect(std::none_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code.starts_with("GTI-B");
                      }),
         std::string("diagnosed at source: ") + declarations + body);
}

// An implicit conversion is a construction too: its one expression node is
// both the argument and the converted value, and it publishes the argument's
// owner as an explicit construction does (L-OWN-02).
void testContextualConstruction() {
  const auto result = analyzeText(kConversions + R"gti(
int main() {
  std::string input{"converted"};
  std::optional<std::string_view> maybe = input.view();
  mut uint64_t total = 0;
  if (maybe) {
    total = total + (*maybe).size();
  }
  std::string_view view = input.view();
  Token token = Token(view, 1);
  expected<int, std::string_view> failed = unexpected(input.view());
  if (!failed) {
    total = total + failed.error().size();
  }
  std::unexpected_value<std::string_view> failure = unexpected(input.view());
  total = total + token.text.size() + std::move(failure).error().size();
  // An explicit construction from a moved failure re-roots its text rather
  // than continuing the failure's loan.
  std::unexpected_value<std::string_view> named = unexpected(input.view());
  expected<int, std::string_view> rebuilt =
      expected<int, std::string_view>(std::move(named));
  if (!rebuilt) {
    total = total + rebuilt.error().size();
  }
  return total == 45 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "implicit conversions keep the owner's text");

  struct Case {
    const char *body;
    const char *code;
  };
  for (const Case &entry : {
           Case{"std::optional<std::string_view> maybe = owner.view(); "
                "kept = *maybe;",
                "GTI-S2045"},
           Case{"std::optional<std::string_view> maybe = owner.view(); "
                "owner.append(\"!\"); uint64_t n = (*maybe).size();",
                "GTI-S2017"},
           Case{"std::string_view view = owner.view(); "
                "std::optional<std::string_view> maybe = view; "
                "owner.append(\"!\"); uint64_t n = (*maybe).size();",
                "GTI-S2017"},
           Case{"std::string_view view = owner.view(); Token token = "
                "Token(view, 1); owner.append(\"!\"); uint64_t n = "
                "token.text.size();",
                "GTI-S2017"},
           Case{"expected<int, std::string_view> failed = "
                "unexpected(owner.view()); kept = failed.error();",
                "GTI-S2045"},
           Case{"expected<int, std::string_view> failed = "
                "unexpected(owner.view()); std::string_view text = "
                "failed.error(); owner.append(\"!\"); uint64_t n = "
                "text.size();",
                "GTI-S2017"},
           Case{"std::unexpected_value<std::string_view> failure = "
                "unexpected(owner.view()); kept = std::move(failure).error();",
                "GTI-S2045"},
           Case{
               "std::unexpected_value<std::string_view> failure = "
               "unexpected(owner.view()); expected<int, std::string_view> "
               "rebuilt = expected<int, std::string_view>(std::move(failure)); "
               "kept = rebuilt.error();",
               "GTI-S2045"},
       })
    expectConversionRejected("", entry.body, entry.code);
  // A conversion of a local owner's view does not leave its body.
  expectConversionRejected(
      "std::optional<std::string_view> leak() { std::string text{\"x\"}; "
      "return text.view(); }\n",
      "std::optional<std::string_view> maybe = leak();", "GTI-S2045");
}

// A constructor that stores a by-value class formal's text field, directly
// or through a zero-argument accessor, re-roots the argument's dependency at
// that field to the stored path of the new value.
void testFieldRelays() {
  const auto result = analyzeText(kConversions + R"gti(
int main() {
  std::string input{"relayed"};
  Token token = Token(input.view(), 1);
  Wrap wrap = Wrap(token);
  mut uint64_t total = 0;
  switch (wrap.word) {
  case Word::text(value): total = total + value.size(); break;
  case Word::none: break;
  }
  Copy copy = Copy(Token(input.view(), 2));
  Copy moved = Copy(std::move(token));
  Named named = Named(Token(input.view(), 3));
  total = total + copy.text.size() + moved.text.size() + named.text.size();
  return total == 28 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "constructors relay a formal's text field");
  if (!result.canGenerateCode())
    return;
  // MIR recomputes each relayed path from the constructor's initializers.
  std::size_t relays = 0;
  for (const auto &location : publications(result.mir)) {
    auto probe = result.mir;
    const auto &instruction = instructionAt(probe, location);
    if (instruction.kind != lang::MirInstructionKind::Construct ||
        instruction.borrowFields.size() != 1 ||
        instruction.borrowFields.front().sourceFields.size() != 1)
      continue;
    ++relays;
    const auto source = instruction.borrowFields.front().sourceFields.front();
    auto movedPath = result.mir;
    instructionAt(movedPath, location)
        .borrowFields.front()
        .resultFields.push_back(source);
    expect(!lang::verifyMirProgram(movedPath).valid(),
           "MIR rejects a relay published at another path");
    auto otherSource = result.mir;
    instructionAt(otherSource, location)
        .borrowFields.front()
        .sourceFields.front() = 0;
    expect(!lang::verifyMirProgram(otherSource).valid(),
           "MIR rejects a relay from a field the constructor does not store");
  }
  // parse's conversion of its unexpected_value into expected relays too.
  expect(relays == 5, "each constructor relay publishes its stored path");

  struct Case {
    const char *body;
    const char *code;
  };
  for (const Case &entry : {
           Case{"Token token = Token(owner.view(), 1); Copy copy = "
                "Copy(token); kept = copy.text;",
                "GTI-S2045"},
           Case{"Token token = Token(owner.view(), 1); Wrap wrap = "
                "Wrap(token); switch (wrap.word) { case Word::text(value): "
                "kept = value; break; case Word::none: break; }",
                "GTI-S2045"},
           Case{"Token token = Token(owner.view(), 1); Wrap wrap = "
                "Wrap(token); owner.append(\"!\"); switch (wrap.word) { case "
                "Word::text(value): break; case Word::none: break; }",
                "GTI-S2017"},
           Case{"Token token = Token(owner.view(), 1); Leaky leaky = "
                "Leaky(token);",
                "GTI-S2045"},
           Case{"Token token = Token(owner.view(), 1); Twice twice = "
                "Twice(token);",
                "GTI-S2045"},
           Case{"Token token = Token(owner.view(), 1); "
                "keepToken(std::move(token));",
                "GTI-S2045"},
       })
    expectConversionRejected("", entry.body, entry.code);
}

// A function returning a value over its one read-only reference formal's
// referent relays it: each call's result views that call's argument, as
// `parse(std::string&)` returning `unexpected(input.view())` needs.
void testDependentReturns() {
  const auto result = analyzeText(kConversions + R"gti(
int main() {
  std::string input{"returned"};
  mut uint64_t total = 0;
  expected<int, std::string_view> parsed = parse(input);
  if (!parsed) {
    total = total + parsed.error().size();
  }
  expected<int, std::string_view> again = parseAgain(input);
  if (!again) {
    total = total + again.error().size();
  }
  Token token = tokenOf(input);
  Token local = viaLocal(input);
  Word word = wordOf(input);
  switch (word) {
  case Word::text(value): total = total + value.size(); break;
  case Word::none: break;
  }
  std::optional<std::string_view> found = find(input);
  if (found) {
    total = total + (*found).size();
  }
  std::string empty{""};
  expected<int, std::string_view> value = parse(empty);
  total = total + token.text.size() + local.text.size() +
          uint64_t(value.value());
  return total == 48 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "returned values view a reference formal's referent");
  if (!result.canGenerateCode())
    return;
  // MIR checks each function's return summary against its body, and admits
  // an ownerless alternative such as `return 0;` only under the summary.
  std::size_t summaries = 0;
  for (const auto &function : result.mir.functionInstances()) {
    if (!function.dependentTextReturn)
      continue;
    ++summaries;
    auto cleared = result.mir;
    const_cast<lang::MirFunctionInstance *>(
        cleared.findFunctionInstance(function.id))
        ->dependentTextReturn = false;
    expect(!lang::verifyMirProgram(cleared).valid(),
           "MIR rejects a dependent return without its summary");
    auto pathless = result.mir;
    const_cast<lang::MirFunctionInstance *>(
        pathless.findFunctionInstance(function.id))
        ->returnBorrowFields.front()
        .resultFields.clear();
    expect(!lang::verifyMirProgram(pathless).valid(),
           "MIR rejects a dependent return summary without its text path");
  }
  expect(summaries == 6, "every returning function records its summary");

  struct Case {
    const char *declarations;
    const char *body;
    const char *code;
  };
  for (const Case &entry : {
           Case{"",
                "expected<int, std::string_view> parsed = parse(owner); "
                "kept = parsed.error();",
                "GTI-S2045"},
           Case{"",
                "expected<int, std::string_view> parsed = parse(owner); "
                "owner.append(\"!\"); uint64_t n = parsed.error().size();",
                "GTI-S2017"},
           Case{"",
                "expected<int, std::string_view> parsed = parse(owner); "
                "std::string moved = std::move(owner); uint64_t n = "
                "parsed.error().size();",
                "GTI-S2017"},
           Case{"",
                "expected<int, std::string_view> parsed = parse(owner); "
                "std::string_view text = parsed.error(); "
                "owner.append(\"!\"); uint64_t n = text.size();",
                "GTI-S2017"},
           Case{"",
                "expected<int, std::string_view> parsed = "
                "parse(std::string(\"temporary\"));",
                "GTI-S2045"},
           Case{"",
                "Word word = wordOf(owner); switch (word) { case "
                "Word::text(value): kept = value; break; case Word::none: "
                "break; }",
                "GTI-S2045"},
           Case{"Token localOwner() { std::string text{\"x\"}; return "
                "Token(text.view(), 1); }\n",
                "Token token = localOwner();", "GTI-S2045"},
           Case{"Token byValue(std::string text) { return Token(text.view(), "
                "1); }\n",
                "Token token = byValue(std::string(\"x\"));", "GTI-S2045"},
           Case{"Token mutated(mut std::string& text) { return "
                "Token(text.view(), 1); }\n",
                "Token token = mutated(owner);", "GTI-S2045"},
           Case{"Token pick(std::string& a, std::string& b, bool first) { if "
                "(first) { return Token(a.view(), 1); } return "
                "Token(b.view(), 2); }\n",
                "Token token = pick(owner, owner, true);", "GTI-S2045"},
           Case{"expected<int, std::string_view> outer() { std::string "
                "text{\"x\"}; return parse(text); }\n",
                "expected<int, std::string_view> parsed = outer();",
                "GTI-S2045"},
       })
    expectConversionRejected(entry.declarations, entry.body, entry.code);
}

const std::string kPayloadUses = R"gti(
import std.optional;
import std.string;
mut std::string_view kept = "";
enum Word { text(std::string_view value), none };
mut Word keptWord = Word::none;
void keepWord(Word word) {
  switch (word) {
  case Word::text(value): kept = value; break;
  case Word::none: break;
  }
}
void keepWordRef(Word& word) {
  switch (word) {
  case Word::text(value): kept = value; break;
  case Word::none: break;
  }
}
class Line {
public:
  mut Word word;
  Line(std::string_view text) : word(Word::text(text)) {}
  bool hasText() {
    switch (this.word) {
    case Word::text: return true;
    case Word::none: return false;
    }
  }
  void copyOut() {
    Word word = this.word;
    switch (word) {
    case Word::text(value): kept = value; break;
    case Word::none: break;
    }
  }
  void passOut() { keepWord(this.word); }
  void lendOut() { keepWordRef(this.word); }
  void storeOut() { keptWord = this.word; }
  Word& wordRef() { return this.word; }
  void consume() && {
    switch (std::move(this.word)) {
    case Word::text(value): kept = value; break;
    case Word::none: break;
    }
  }
};
void copyLine(Line line) {
  Word word = line.word;
  switch (word) {
  case Word::text(value): kept = value; break;
  case Word::none: break;
  }
}
class Box {
public:
  mut std::string_view text;
  Box(std::string_view text) : text(text) {}
  void keep() && { kept = std::move(this.text); }
  std::string_view take() && { return std::move(this.text); }
};
class Cell<T> {
public:
  T value;
  Cell(T value) : value(value) {}
  T get() { return this.value; }
  T& ref() { return this.value; }
  uint64_t copy() {
    T held = this.value;
    return 1;
  }
};
)gti";

// A payload-enum field of a contract formal or the receiver is observed in
// place by a switch on it. Any other use, a copy, move, borrow or store of
// the field, may carry any of its text payloads away, so each is required
// ownerless at every call (L-OWN-02). A consuming receiver's moved text
// field forwards or relays like a read of it.
void testPayloadFieldUses() {
  const auto result = analyzeText(kPayloadUses + R"gti(
int main() {
  mut Line line = Line("literal");
  line.copyOut();
  line.passOut();
  line.lendOut();
  line.storeOut();
  keepWordRef(line.wordRef());
  copyLine(line);
  std::move(line).consume();
  mut Box box = Box("boxed");
  std::move(box).keep();
  std::string input{"relayed"};
  mut Box relayed = Box(input.view());
  std::optional<std::string_view> maybe = std::optional<std::string_view>("x");
  std::string_view taken = std::move(relayed).take();
  std::string_view value = std::move(maybe).value();
  // A type-parameter field is a payload enum only in this instance, whose
  // analysis records what its MIR derives.
  Cell<Word> cell = Cell<Word>(Word::text("cell"));
  Word got = cell.get();
  Word& held = cell.ref();
  return kept.size() + taken.size() + value.size() + cell.copy() == 14 ? 0
                                                                       : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "ownerless values copy, pass and store their payload fields");
  if (!result.canGenerateCode())
    return;
  // MIR derives each payload requirement from the field's use; the in-place
  // switch of hasText requires nothing, and neither do the accessors
  // `wordRef` and `Cell<Word>::ref`, whose callers read the field they
  // return (a caller's non-switch use requires its payloads instead).
  std::size_t payloadUses = 0;
  for (const auto &function : result.mir.functionInstances()) {
    if (function.definitionKind == lang::MirDefinitionKind::Source &&
        function.owner &&
        function.returnType.kind == lang::SemanticType::Reference &&
        function.returnType.arguments.size() == 1 &&
        function.returnType.arguments.front().kind == lang::SemanticType::Enum)
      expect(function.ownerlessTextFields.empty(),
             "a payload-enum field accessor relays its field");
    if (std::none_of(function.ownerlessTextFields.begin(),
                     function.ownerlessTextFields.end(),
                     [](const auto &field) { return field.payload != 0; }))
      continue;
    ++payloadUses;
    auto changed = result.mir;
    const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(function.id))
        ->ownerlessTextFields.clear();
    expect(!lang::verifyMirProgram(changed).valid(),
           "MIR derives a payload field use's requirement independently");
  }
  expect(payloadUses == 9, "every payload field use requires its payload: " +
                               std::to_string(payloadUses));

  struct Case {
    const char *body;
    const char *code;
  };
  for (const Case &entry : {
           Case{"Line line = Line(owner.view()); line.copyOut();", "GTI-S2045"},
           Case{"Line line = Line(owner.view()); line.passOut();", "GTI-S2045"},
           Case{"Line line = Line(owner.view()); line.lendOut();", "GTI-S2045"},
           Case{"Line line = Line(owner.view()); line.storeOut();",
                "GTI-S2045"},
           Case{"Line line = Line(owner.view()); copyLine(line);", "GTI-S2045"},
           Case{
               "mut Line line = Line(owner.view()); std::move(line).consume();",
               "GTI-S2045"},
           Case{"mut Box box = Box(owner.view()); std::move(box).keep();",
                "GTI-S2045"},
           Case{"mut Box box = Box(owner.view()); kept = "
                "std::move(box).take();",
                "GTI-S2045"},
           Case{"mut Box box = Box(owner.view()); std::string_view text = "
                "std::move(box).take(); owner.append(\"!\"); uint64_t n = "
                "text.size();",
                "GTI-S2017"},
           Case{"mut std::optional<std::string_view> maybe = "
                "std::optional<std::string_view>(owner.view()); kept = "
                "std::move(maybe).value();",
                "GTI-S2045"},
       }) {
    const auto rejected = analyzeText(
        kPayloadUses + "int run() { mut std::string owner{\"owner\"}; " +
        entry.body + " return 0; }\nint main() { return run(); }\n");
    expect(hasDiagnosticCode(rejected.diagnostics, entry.code),
           std::string("a dependent payload field use is checked at source: ") +
               entry.body);
    expect(std::none_of(rejected.diagnostics.begin(),
                        rejected.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           std::string("diagnosed at source: ") + entry.body);
  }
}

const std::string kNesting = R"gti(
import std.optional;
import std.string;
mut std::string_view kept = "";
class Diag {
public:
  std::string_view text;
  int line;
  Diag(std::string_view text, int line) : text(text), line(line) {}
};
class Report {
public:
  Diag diag;
  int code;
  Report(Diag diag, int code) : diag(diag), code(code) {}
};
class Outer {
public:
  Report report;
  Outer(Report report) : report(report) {}
};
class Token {
public:
  std::string_view text;
  int kind;
  Token(std::string_view text, int kind) : text(text), kind(kind) {}
};
expected<int, Diag> check(std::string& input) {
  if (input.empty()) {
    return 0;
  }
  return unexpected(Diag(input.view(), 3));
}
void keepReport(Report report) { kept = report.diag.text; }
void keepDeref(std::optional<Token>& maybe) { kept = (*maybe).text; }
void keepCopy(std::optional<Token>& maybe) {
  Token token = *maybe;
  kept = token.text;
}
uint64_t width(std::optional<Token>& maybe) { return (*maybe).text.size(); }
void keepError(expected<int, Diag>& result) { kept = result.error().text; }
uint64_t errorWidth(expected<int, Diag>& result) {
  return result.error().text.size();
}
)gti";

// A dependent value stored whole in a field or payload of another value
// nests its text path there (L-OWN-02 phase 3): a user class, an optional of
// a class and an expected whose error is a class, built and returned over a
// caller's string, and read through fields and accessor references.
void testNestedValues() {
  const auto result = analyzeText(kNesting + R"gti(
int main() {
  std::string input{"nested"};
  Diag diag = Diag(input.view(), 1);
  Report report = Report(diag, 2);
  Outer outer = Outer(Report(Diag(input.view(), 3), 4));
  mut uint64_t total = report.diag.text.size() + outer.report.diag.text.size();
  std::optional<Token> maybe = Token(input.view(), 5);
  if (maybe) {
    total = total + (*maybe).text.size() + maybe.value().text.size() +
            width(maybe);
  }
  expected<int, Diag> failed = unexpected(Diag(input.view(), 6));
  expected<int, Diag> checked = check(input);
  if (!failed && !checked) {
    total = total + failed.error().text.size() + errorWidth(checked);
  }
  std::string empty{""};
  expected<int, Diag> passed = check(empty);
  Report literal = Report(Diag("literal", 7), 8);
  keepReport(literal);
  std::optional<Token> ownerless = Token("ownerless", 9);
  keepDeref(ownerless);
  keepCopy(ownerless);
  return total + uint64_t(passed.value()) == 42 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "nested dependent values keep their owner's text");
  if (!result.canGenerateCode())
    return;
  // MIR recomputes each nested publication's path: a whole value stored in a
  // field, a class formal returned whole and a class field re-rooted through
  // its accessor.
  std::size_t nested = 0;
  for (const auto &function : result.mir.functionInstances())
    for (std::size_t block = 0; block < function.body.blocks.size(); ++block)
      for (std::size_t index = 0;
           index < function.body.blocks[block].instructions.size(); ++index) {
        const auto &instruction =
            function.body.blocks[block].instructions[index];
        if (!instruction.dependentTextPublication ||
            instruction.borrowFields.size() != 1 ||
            instruction.borrowFields.front().resultFields.size() < 2 ||
            instruction.borrowFields.front().sourceFields.empty())
          continue;
        ++nested;
        auto changed = result.mir;
        auto *target = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        target->body.blocks[block]
            .instructions[index]
            .borrowFields.front()
            .resultFields.pop_back();
        expect(!lang::verifyMirProgram(changed).valid(),
               "MIR rejects a nested publication missing its leaf");
      }
  expect(nested >= 5, "each nesting publishes its whole path");
  // A function reading text under a formal's nested value requires the
  // formal's prefix ownerless, as MIR derives.
  std::size_t prefixes = 0;
  for (const auto &function : result.mir.functionInstances())
    if (std::any_of(function.ownerlessTextFields.begin(),
                    function.ownerlessTextFields.end(),
                    [](const auto &field) { return field.payload != 0; }) &&
        function.parameterTypes.size() == 1 &&
        function.parameterTypes.front().kind == lang::SemanticType::Reference) {
      ++prefixes;
      auto changed = result.mir;
      const_cast<lang::MirFunctionInstance *>(
          changed.findFunctionInstance(function.id))
          ->ownerlessTextFields.clear();
      expect(!lang::verifyMirProgram(changed).valid(),
             "MIR derives a nested payload's requirement independently");
    }
  expect(prefixes >= 3, "keepDeref, keepCopy and keepError require prefixes");

  struct Case {
    const char *body;
    const char *code;
  };
  for (const Case &entry : {
           Case{"Report r = Report(Diag(owner.view(), 1), 2); kept = "
                "r.diag.text;",
                "GTI-S2045"},
           Case{"Report r = Report(Diag(owner.view(), 1), 2); Diag d = "
                "r.diag;",
                "GTI-S2045"},
           Case{"Report r = Report(Diag(owner.view(), 1), 2); "
                "owner.append(\"!\"); uint64_t n = r.diag.text.size();",
                "GTI-S2017"},
           Case{"Report r = Report(Diag(owner.view(), 1), 2); keepReport(r);",
                "GTI-S2045"},
           Case{"std::optional<Token> o = Token(owner.view(), 1); kept = "
                "(*o).text;",
                "GTI-S2045"},
           Case{"std::optional<Token> o = Token(owner.view(), 1); Token t = "
                "*o;",
                "GTI-S2045"},
           Case{"std::optional<Token> o = Token(owner.view(), 1); "
                "keepDeref(o);",
                "GTI-S2045"},
           Case{"std::optional<Token> o = Token(owner.view(), 1); "
                "keepCopy(o);",
                "GTI-S2045"},
           Case{"std::optional<Token> o = Token(owner.view(), 1); "
                "std::string_view v = o.value().text; owner.append(\"!\"); "
                "uint64_t n = v.size();",
                "GTI-S2017"},
           Case{"expected<int, Diag> r = check(owner); kept = "
                "r.error().text;",
                "GTI-S2045"},
           Case{"expected<int, Diag> r = check(owner); Diag d = r.error();",
                "GTI-S2045"},
           Case{"expected<int, Diag> r = check(owner); keepError(r);",
                "GTI-S2045"},
           Case{"expected<int, Diag> r = check(owner); owner.append(\"!\"); "
                "uint64_t n = r.error().text.size();",
                "GTI-S2017"},
       }) {
    const auto rejected = analyzeText(
        kNesting + "int run() { mut std::string owner{\"owner\"}; " +
        entry.body + " return 0; }\nint main() { return run(); }\n");
    expect(hasDiagnosticCode(rejected.diagnostics, entry.code),
           std::string("a nested dependent value is checked at source: ") +
               entry.body);
    expect(std::none_of(rejected.diagnostics.begin(),
                        rejected.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           std::string("diagnosed at source: ") + entry.body);
  }
}

const std::string kCarrierDeclarations = R"gti(
import std.string;
class Token {
public:
  std::string_view text;
  int kind;
  Token(std::string_view text, int kind) : text(text), kind(kind) {}
  std::string_view name() { return this.text; }
  std::string_view& ref() { return this.text; }
  int& kindRef() { return this.kind; }
};
enum Word { text(std::string_view value), count(int value) };
class Line {
public:
  Word word;
  Line(std::string_view text) : word(Word::text(text)) {}
};
std::string_view first(Token token) { return token.text; }
std::string_view id(std::string_view text) { return text; }
)gti";

// MIR derives which local bindings hold a published owner's text from the
// publications themselves, not from the carriers semantics recorded
// (L-OWN-02): copies and moves of the value, reads of its designated field,
// bound payloads (also through a field's payload enum), relay and formal
// relay results, and views copied through a returned reference. Each must
// carry a loan that retains the owner; an int read through the value's
// reference holds no text.
void testDerivedCarriers() {
  const auto result = analyzeText(kCarrierDeclarations + R"gti(
int main() {
  std::string input{"carried"};
  Token token = Token(input.view(), 1);
  Token copy = token;
  Token moved = std::move(copy);
  std::string_view field = moved.text;
  std::string_view again = field;
  std::string_view relayed = first(token);
  std::string_view method = token.name();
  std::string_view formal = id(field);
  std::string_view referenced = token.ref();
  std::string_view temporary = first(Token(input.view(), 2));
  int kind = token.kindRef();
  Word word = Word::text(input.view());
  Line line = Line(input.view());
  mut uint64_t total = field.size() + again.size() + relayed.size() +
                       method.size() + formal.size() + referenced.size() +
                       temporary.size();
  switch (word) {
  case Word::text(value): total = total + value.size(); break;
  case Word::count(value): break;
  }
  switch (line.word) {
  case Word::text(value): total = total + value.size(); break;
  case Word::count(value): break;
  }
  return total + uint64_t(kind) == 64 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "each view of a published owner verifies with its carriers");
  const auto published = publications(result.mir);
  if (!result.canGenerateCode() || published.empty())
    return;
  const auto function = published.front().function;
  const auto &loans = result.mir.findFunctionInstance(function)->body.loans;
  const auto verifyChanged = [&](const auto &change) {
    auto changed = result.mir;
    change(const_cast<lang::MirFunctionInstance *>(
               changed.findFunctionInstance(function))
               ->body.loans);
    return lang::verifyMirProgram(changed);
  };
  const auto unretained = [](const lang::MirVerificationResult &verified) {
    return std::any_of(
        verified.errors.begin(), verified.errors.end(), [](const auto &error) {
          return error.message.find("does not carry a loan that retains its "
                                    "owner") != std::string::npos;
        });
  };
  // The first `input.view()` call's own loan names the same owner but
  // retains no publication.
  expect(!loans.empty() && loans.front().kind == lang::MirLoanKind::Stored &&
             loans.front().designatedFields.empty() &&
             loans.front().carriers.empty(),
         "the owner's first view has an uncarried loan");
  // Dropping a binding from a loan it carries, or moving it onto that
  // unrelated loan, leaves the text it holds unretained.
  std::size_t carriers = 0;
  for (std::size_t loan = 1; loan < loans.size(); ++loan)
    for (std::size_t index = 0; index < loans[loan].carriers.size(); ++index) {
      ++carriers;
      const auto binding = loans[loan].carriers[index];
      expect(unretained(verifyChanged([&](auto &changed) {
               auto &list = changed[loan].carriers;
               list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
             })),
             "a binding holding a published owner's text must carry it");
      expect(unretained(verifyChanged([&](auto &changed) {
               auto &list = changed[loan].carriers;
               list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
               changed.front().carriers.push_back(binding);
             })),
             "another loan on the same owner does not retain a publication");
    }
  expect(carriers >= 14, "every binding over the owner is checked");
  // The view copied through `token.ref()` carries the call's result loan,
  // which retains the owner only through its stored dependency.
  const auto relayed =
      std::find_if(loans.begin(), loans.end(), [](const auto &loan) {
        return loan.kind == lang::MirLoanKind::CallResult &&
               loan.storedDependency != 0 && !loan.carriers.empty();
      });
  expect(relayed != loans.end(), "a reference relay's view carries its loan");
  if (relayed != loans.end()) {
    const auto index = static_cast<std::size_t>(relayed - loans.begin());
    expect(unretained(verifyChanged(
               [&](auto &changed) { changed[index].storedDependency = 0; })),
           "a reference relay's loan retains the owner through its stored "
           "dependency");
  }
}

// A reference a call returns into a value holding dynamic text from a local
// owner is that value's dependent field (L-OWN-02), as `value.field` is: a
// switch observes it in place, through the call or a reference local bound
// to it, but it cannot carry the text out. Before 0.468.1 every escape below
// compiled, and `leak` returned freed text.
const std::string kReferenceResults = R"gti(
import std.string;
mut std::string_view kept = "";
enum Word { text(std::string_view value), none };
mut Word keptWord = Word::none;
void keepWordRef(Word& word) {
  switch (word) {
  case Word::text(value): kept = value; break;
  case Word::none: break;
  }
}
void keepWord(Word word) {
  switch (word) {
  case Word::text(value): kept = value; break;
  case Word::none: break;
  }
}
void readWordRef(Word& word) {
  switch (word) {
  case Word::text(auto& value): std::println(value); break;
  case Word::none: break;
  }
}
class Line {
public:
  Word word;
  Word other;
  Line(std::string_view value) : word(Word::text(value)), other(Word::none) {}
  Word& wordRef() { return this.word; }
  Word& either(bool first) {
    if (first) { return this.word; }
    return this.other;
  }
  void keepEither() { keepWordRef(this.either(true)); }
  void showEither() {
    switch (this.either(true)) {
    case Word::text(value): std::println(value); break;
    case Word::none: break;
    }
  }
};
Word& pickWord(Line& line) { return line.word; }
void keepFrom(Line& line) { keepWordRef(line.either(true)); }
void keepSwitch(Line& line) {
  switch (line.either(true)) {
  case Word::text(value): kept = value; break;
  case Word::none: break;
  }
}
)gti";

void testDependentReferenceResults() {
  const auto accepted = analyzeText(kReferenceResults + R"gti(
int run() {
  std::string input{"owned"};
  Line owned = Line(input.view());
  switch (owned.wordRef()) {
  case Word::text(value): std::println(value); break;
  case Word::none: return 1;
  }
  Word& word = owned.wordRef();
  Word& again = word;
  switch (again) {
  case Word::text(auto& value): std::println(value); break;
  case Word::none: return 2;
  }
  // A reference to one of several fields is dependent on each of them.
  switch (owned.either(false)) {
  case Word::text(value): std::println(value); break;
  case Word::none: break;
  }
  Word& chosen = owned.either(true);
  switch (chosen) {
  case Word::text(value): std::println(value); break;
  case Word::none: return 3;
  }
  // Over literal text the same references may leave.
  Line literal = Line("literal");
  keepWordRef(literal.wordRef());
  keepWord(literal.wordRef());
  keepWordRef(pickWord(literal));
  Word& held = literal.wordRef();
  keepWordRef(held);
  keptWord = literal.wordRef();
  keepWordRef(literal.either(true));
  keepFrom(literal);
  keepSwitch(literal);
  literal.keepEither();
  literal.showEither();
  return 0;
}
int main() { return run(); }
)gti");
  if (!accepted.canGenerateCode())
    printDiagnostics(accepted);
  expect(accepted.canGenerateCode() &&
             lang::verifyMirProgram(accepted.mir).valid(),
         "a dependent reference result can be observed in place, and an "
         "ownerless one can leave");
  // `either` relays both fields and requires nothing. A formal's call of it
  // reads both, switch included, since a payload binding cannot name the
  // field; MIR derives each such requirement independently.
  std::size_t severalFieldUses = 0;
  for (const auto &function : accepted.mir.functionInstances()) {
    const auto *info = accepted.semantics.findFunction(function.declaration);
    const std::string name = info == nullptr ? "" : info->qualifiedName;
    if (name.ends_with("either"))
      expect(function.ownerlessTextFields.empty(),
             "an accessor of several fields relays them");
    if (!name.ends_with("keepFrom") && !name.ends_with("keepSwitch") &&
        !name.ends_with("keepEither") && !name.ends_with("showEither"))
      continue;
    ++severalFieldUses;
    std::set<lang::SymbolId> fields;
    for (const auto &field : function.ownerlessTextFields)
      fields.insert(field.field);
    expect(fields.size() == 2,
           "a use of an accessor of several fields reads each: " + name);
    auto changed = accepted.mir;
    const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(function.id))
        ->ownerlessTextFields.clear();
    expect(!lang::verifyMirProgram(changed).valid(),
           "MIR derives a several-field accessor use's requirement: " + name);
  }
  expect(severalFieldUses == 4, "every several-field accessor use is checked");

  // Passing a formal's accessor result uses the field it designates, not the
  // whole formal: another text field may hold a local owner's text. MIR used
  // to require every field and reported GTI-B0001.
  const auto designated = analyzeText(kReferenceResults + R"gti(
class Pair {
public:
  Word word;
  Word spare;
  Pair(std::string_view first, std::string_view second)
      : word(Word::text(first)), spare(Word::text(second)) {}
  Word& wordRef() { return this.word; }
};
void keepPairWord(Pair& pair) { keepWordRef(pair.wordRef()); }
int run() {
  std::string input{"owned"};
  Pair pair = Pair("literal", input.view());
  keepPairWord(pair);
  return 0;
}
int main() { return run(); }
)gti");
  if (!designated.canGenerateCode())
    printDiagnostics(designated);
  expect(designated.canGenerateCode() &&
             lang::verifyMirProgram(designated.mir).valid(),
         "a passed accessor result requires only its designated field");
  bool sawKeep = false;
  for (const auto &function : designated.mir.functionInstances()) {
    const auto *info = designated.semantics.findFunction(function.declaration);
    if (info == nullptr || info->qualifiedName != "keepPairWord")
      continue;
    sawKeep = true;
    expect(!function.ownerlessTextFields.empty() &&
               std::all_of(function.ownerlessTextFields.begin(),
                           function.ownerlessTextFields.end(),
                           [&](const auto &field) {
                             return field.field ==
                                    function.ownerlessTextFields.front().field;
                           }),
           "keepPairWord requires the word field alone");
    auto changed = designated.mir;
    const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(function.id))
        ->ownerlessTextFields.clear();
    expect(!lang::verifyMirProgram(changed).valid(),
           "MIR derives the designated field's requirement");
  }
  expect(sawKeep, "keepPairWord is lowered");

  struct Case {
    const char *body;
    const char *code;
  };
  for (const Case &entry : {
           Case{"keepWordRef(owned.wordRef());", "GTI-S2045"},
           Case{"keepWord(owned.wordRef());", "GTI-S2045"},
           Case{"readWordRef(owned.wordRef());", "GTI-S2045"},
           Case{"Word copy = owned.wordRef();", "GTI-S2045"},
           Case{"keptWord = owned.wordRef();", "GTI-S2045"},
           Case{"Word& word = owned.wordRef(); keepWordRef(word);",
                "GTI-S2045"},
           Case{"Word& word = owned.wordRef(); Word copy = word;", "GTI-S2045"},
           Case{"Word& word = owned.wordRef(); Word copy = std::move(word);",
                "GTI-S2045"},
           Case{"Word& word = owned.wordRef(); Word& again = word; "
                "keepWordRef(again);",
                "GTI-S2045"},
           Case{"Word& word = owned.wordRef(); keptWord = word;", "GTI-S2045"},
           Case{"keepWordRef(pickWord(owned));", "GTI-S2045"},
           Case{"Word& word = pickWord(owned); keepWordRef(word);",
                "GTI-S2045"},
           Case{"keepWordRef(owned.either(true));", "GTI-S2045"},
           Case{"Word& word = owned.either(false); keepWordRef(word);",
                "GTI-S2045"},
           Case{"Word copy = owned.either(true);", "GTI-S2045"},
           Case{"keepFrom(owned);", "GTI-S2045"},
           Case{"keepSwitch(owned);", "GTI-S2045"},
           Case{"owned.keepEither();", "GTI-S2045"},
           // Conservative: the switch reads both fields (see above).
           Case{"owned.showEither();", "GTI-S2045"},
           Case{"Word& word = owned.wordRef(); input.append(\"!\"); "
                "switch (word) { case Word::text(value): "
                "std::println(value); break; case Word::none: break; }",
                "GTI-S2017"},
       }) {
    const auto rejected = analyzeText(
        kReferenceResults +
        "int run() { mut std::string input{\"owned\"}; Line owned = "
        "Line(input.view()); " +
        entry.body + " return 0; }\nint main() { return run(); }\n");
    expect(hasDiagnosticCode(rejected.diagnostics, entry.code),
           std::string("a dependent reference result cannot carry its text "
                       "out: ") +
               entry.body);
    expect(std::none_of(rejected.diagnostics.begin(),
                        rejected.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           std::string("diagnosed at source: ") + entry.body);
  }

  // Returning a copy of the referent from the owner's own function would
  // return freed text.
  const auto leaked = analyzeText(kReferenceResults + R"gti(
Word leak() {
  std::string input{"freed"};
  Line owned = Line(input.view());
  return owned.wordRef();
}
int main() { return 0; }
)gti");
  expect(hasDiagnosticCode(leaked.diagnostics, "GTI-S2045") &&
             !leaked.canGenerateCode(),
         "a copy of a dependent reference result cannot be returned");

  // A reference whose returns are not field paths of the owner cannot be
  // tracked, so it is refused over dynamic text.
  const auto untracked = analyzeText(kReferenceResults + R"gti(
class Holder {
public:
  Line line;
  Holder(Line line) : line(line) {}
  Word& viaLocal() {
    Word& found = this.line.word;
    return found;
  }
};
int run() {
  std::string input{"owned"};
  Holder holder = Holder(Line(input.view()));
  switch (holder.viaLocal()) {
  case Word::text(value): std::println(value); break;
  case Word::none: break;
  }
  return 0;
}
int main() { return run(); }
)gti");
  expect(hasDiagnosticCode(untracked.diagnostics, "GTI-S2045") &&
             std::none_of(untracked.diagnostics.begin(),
                          untracked.diagnostics.end(),
                          [](const auto &diagnostic) {
                            return diagnostic.code.starts_with("GTI-B");
                          }),
         "an untracked reference into dynamic text is refused at source");
}

constexpr NamedTest kTests[] = {
    {"testDependentPublication", testDependentPublication},
    {"testOwnerlessConstructionUnchanged", testOwnerlessConstructionUnchanged},
    {"testConfinement", testConfinement},
    {"testRelays", testRelays},
    {"testConstructorReads", testConstructorReads},
    {"testFieldResults", testFieldResults},
    {"testPayloads", testPayloads},
    {"testCopiedPayloadBindings", testCopiedPayloadBindings},
    {"testTemporaries", testTemporaries},
    {"testPublishedResults", testPublishedResults},
    {"testPublishedResultOrder", testPublishedResultOrder},
    {"testRecursiveRelays", testRecursiveRelays},
    {"testGenericClassContracts", testGenericClassContracts},
    {"testTypeParameterFormals", testTypeParameterFormals},
    {"testPayloadPaths", testPayloadPaths},
    {"testOptionalViews", testOptionalViews},
    {"testContextualConstruction", testContextualConstruction},
    {"testFieldRelays", testFieldRelays},
    {"testDependentReturns", testDependentReturns},
    {"testPayloadFieldUses", testPayloadFieldUses},
    {"testNestedValues", testNestedValues},
    {"testDerivedCarriers", testDerivedCarriers},
    {"testDependentReferenceResults", testDependentReferenceResults},
};
} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, kTests, "view fields");
}
