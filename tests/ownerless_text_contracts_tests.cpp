#include "compiler_test_fixture.h"
#include "gti/hir.h"
#include "gti/mir_verifier.h"

namespace {
void printDiagnostics(const lang::FrontendResult &result) {
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
}
lang::FrontendResult analyzeText(std::string source) {
  return lang::Frontend().analyze("ownerless-text-contracts.gti",
                                  std::move(source), {standardLibraryPrelude()},
                                  {}, {standardLibraryRoot()});
}

void testOwnerlessForwarding() {
  const auto result = analyzeText(R"gti(
#include <std/string>
#include <std/utility>
struct Label<T> {
  mut T text;
  Label(T input) : text(std::move(input)) {}
  T take() && { return std::move(this.text); }
};
Label<T> forward<T>(T input) { return later(std::move(input)); }
Label<T> later<T>(T input) { return Label<T>(std::move(input)); }
Label<std::string_view> uncalled(std::string_view input) { return later(input); }
mut Label<std::string_view> latest = Label<std::string_view>("initial");
void remember(std::string_view input) { latest = later(input); }
int main() {
  auto label = forward("literal");
  auto integer = forward(42);
  remember("remembered");
  return std::move(label).take() == "literal" && std::move(integer).take() == 42 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "conditional ownerless forwarding is generic and order independent");
  if (!result.canGenerateCode())
    return;
  // Returning the fresh label relays the formal (L-OWN-02); storing it in
  // a global still requires ownerless text.
  std::size_t contracts = 0;
  std::size_t relays = 0;
  for (const auto &function : result.mir.functionInstances()) {
    if (!function.publishedTextFields.empty()) {
      ++relays;
      auto changed = result.mir;
      const_cast<lang::MirFunctionInstance *>(
          changed.findFunctionInstance(function.id))
          ->publishedTextFields.clear();
      expect(!lang::verifyMirProgram(changed).valid(),
             "omitted published text relay is rejected");
    }
    if (function.ownerlessTextParameters.empty())
      continue;
    ++contracts;
    auto changed = result.mir;
    auto *target = const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(function.id));
    target->ownerlessTextParameters.clear();
    expect(!lang::verifyMirProgram(changed).valid(),
           "omitted ownerless formal contract is rejected");
  }
  expect(relays >= 2, "forwarding functions relay their text formal");
  expect(contracts >= 1, "a storing function retains its requirement");
  for (const auto &function : result.hir.functionInstances()) {
    if (function.ownerlessTextParameters.empty() &&
        function.publishedTextFields.empty())
      continue;
    auto changed = result.hir;
    auto *target = const_cast<lang::HirFunctionInstance *>(
        changed.findFunctionInstance(function.id));
    target->ownerlessTextContractProof.reset();
    expect(!lang::verifyHirProgramPlans(result.semantics, changed).valid(),
           "omitted semantic ownerless contract seal is rejected");
  }
}

void testOwnerlessTextAliasWrites() {
  const std::string declarations = R"gti(
#include <std/string>
#include <std/utility>
struct Label {
  mut std::string_view text;
  Label() : text("literal") {}
  mut std::string_view& slot() mut { return this.text; }
  std::string_view take() && { return this.text; }
};
void write(mut std::string_view& output, std::string_view input) { output = input; }
)gti";
  for (const char *write :
       {"label.slot() = owner.view();",
        "mut std::string_view& alias = label.slot(); alias = owner.view();",
        "write(label.slot(), owner.view());"}) {
    const auto result = analyzeText(
        declarations +
        "int main() { mut Label label{}; std::string owner{\"dynamic\"}; " +
        write + " return 0; }");
    expect(!result.canGenerateCode(),
           "dynamic text cannot enter a field through mutable aliases");
    expect(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [](const auto &diagnostic) {
                         return diagnostic.code == "GTI-S2045";
                       }),
           "dynamic alias writes are diagnosed at source");
    expect(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           "dynamic alias rejection does not defer to an internal compiler "
           "failure");
  }
  const auto literal = analyzeText(declarations + R"gti(
int main() {
  mut Label label{};
  label.slot() = "replacement";
  write(label.slot(), "another");
  return std::move(label).take() == "another" ? 0 : 1;
}
)gti");
  if (!literal.canGenerateCode())
    printDiagnostics(literal);
  expect(literal.canGenerateCode(),
         "mutable text accessors remain usable with ownerless replacement");
}

bool textResult(const lang::MirFunctionInstance &function) {
  return function.definitionKind == lang::MirDefinitionKind::Source &&
         (function.returnType.kind == lang::SemanticType::StringView ||
          function.returnType.kind == lang::SemanticType::CString);
}

// Whether a body calls a source function with a text result.
bool callsTextResult(const lang::MirProgram &program,
                     const lang::MirBody &body) {
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.functionTarget)
        if (const auto *target =
                program.findFunctionInstance(*instruction.functionTarget);
            target != nullptr && textResult(*target))
          return true;
  return false;
}

bool functionError(const lang::MirVerificationResult &result,
                   lang::HirFunctionInstanceId function,
                   std::string_view reason) {
  return std::any_of(result.errors.begin(), result.errors.end(),
                     [&](const lang::MirVerificationError &error) {
                       return error.bodyKind == lang::MirBodyKind::Function &&
                              error.owner == function &&
                              error.message.find(reason) != std::string::npos;
                     });
}

// The function whose body holds the string literal `text`.
const lang::MirFunctionInstance *
returningLiteral(const lang::MirProgram &program, std::string_view text) {
  for (const auto &function : program.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.operation == lang::MirOperation::Literal &&
            instruction.literal &&
            std::holds_alternative<std::string>(*instruction.literal) &&
            std::get<std::string>(*instruction.literal) == text)
          return &function;
  return nullptr;
}

// Replaces the string literal `text` in a body with a load of the formal bound
// to `formal`, copied from a load the body already performs.
bool loadFormalForLiteral(lang::MirBody &body, lang::HirBindingId formal,
                          std::string_view text) {
  std::optional<lang::MirInstruction> load;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions) {
      const auto *place =
          instruction.kind == lang::MirInstructionKind::Load &&
                  instruction.operands.size() == 1
              ? body.findPlace(instruction.operands.front().place)
              : nullptr;
      if (place && place->root == lang::MirPlaceRootKind::Binding &&
          place->projections.empty() && place->binding == formal)
        load = instruction;
    }
  if (!load)
    return false;
  for (auto &block : body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.operation == lang::MirOperation::Literal &&
          instruction.literal &&
          std::holds_alternative<std::string>(*instruction.literal) &&
          std::get<std::string>(*instruction.literal) == text) {
        const auto identity = instruction.id;
        const auto result = instruction.result;
        instruction = *load;
        instruction.id = identity;
        instruction.result = result;
        return true;
      }
  return false;
}

// Replaces the string literal `text` in a function's body, which each of these
// functions returns, with a load of its first formal.
bool returnFirstFormal(lang::MirProgram &program,
                       lang::HirFunctionInstanceId id, std::string_view text) {
  auto *function =
      const_cast<lang::MirFunctionInstance *>(program.findFunctionInstance(id));
  return loadFormalForLiteral(function->body,
                              function->parameterBindings.front(), text);
}

void testRecursiveOwnerlessReturns() {
  // A recursive call's result holds what the callee's returns hold: the
  // least fixpoint over them, through the call's arguments. Each of these
  // recursions returns only ownerless text.
  for (const char *source : {R"gti(
std::string_view loop(std::string_view s, int n) {
  if (n == 0) {
    return "done";
  }
  return loop(s, n - 1);
}
int main() { return loop("x", 2).size() == 4 ? 0 : 1; }
)gti",
                             R"gti(
c_string loop(c_string s, int n) {
  if (n == 0) {
    return nullptr;
  }
  return loop(s, n - 1);
}
int main() { return loop(nullptr, 2) == nullptr ? 0 : 1; }
)gti",
                             R"gti(
std::string_view even(std::string_view s, int n) {
  return n == 0 ? "even" : odd(s, n - 1);
}
std::string_view odd(std::string_view s, int n) {
  if (n == 0) {
    return "odd";
  }
  return even(s, n - 1);
}
int main() { return even("x", 3).size() == 3 ? 0 : 1; }
)gti",
                             R"gti(
struct Label {
  std::string_view text;
  int line;
};
std::string_view loop(std::string_view s, int n) {
  if (n == 0) {
    return "done";
  }
  return loop(s, n - 1);
}
std::string_view id(std::string_view s) { return s; }
Label make(std::string_view s) {
  Label label{.text = loop(s, 2), .line = 1};
  return label;
}
std::string_view twice() { return id(id("lit")); }
int main() { return make("x").text.size() + twice().size() == 7 ? 0 : 1; }
)gti"}) {
    const auto result = analyzeText(source);
    if (!result.canGenerateCode())
      printDiagnostics(result);
    expect(result.canGenerateCode(),
           "a recursive text result is ownerless when its other returns are, "
           "and may be stored or relayed as such");
    if (!result.canGenerateCode())
      continue;
    expect(std::any_of(result.mir.functionInstances().begin(),
                       result.mir.functionInstances().end(),
                       [&](const auto &function) {
                         return textResult(function) &&
                                function.returnBorrowOrigin ==
                                    lang::BorrowOriginKind::None &&
                                callsTextResult(result.mir, function.body);
                       }),
           "an ownerless text return reaches a text call");
  }
}

void testPermutingRecursiveFieldContracts() {
  const std::string declarations = R"gti(
#include <std/string>
struct Token {
  std::string_view text;
  int line;
};
std::string_view pick(Token a, Token b, int n) {
  if (n == 0) {
    return a.text;
  }
  return pick(b, a, n - 1);
}
std::string_view outer(Token x, Token y) { return pick(x, y, 3); }
)gti";
  const auto result = analyzeText(declarations + R"gti(
int main() {
  Token a{.text = "ab", .line = 1};
  Token b{.text = "xyz", .line = 2};
  return pick(a, b, 1).size() == 3 && outer(a, b).size() == 3 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "a recursion permuting its formals returns ownerless field text");
  if (!result.canGenerateCode())
    return;
  // Some depth of the recursion returns each formal's field, so MIR derives
  // a requirement on both formals; neither may be omitted.
  std::size_t contracts = 0;
  for (const auto &function : result.mir.functionInstances()) {
    const auto &fields = function.ownerlessTextFields;
    if (fields.empty() || function.parameterTypes.size() < 2 ||
        function.parameterTypes[0].kind != lang::SemanticType::Class ||
        function.parameterTypes[0] != function.parameterTypes[1])
      continue;
    ++contracts;
    expect(fields.size() == 2 && !fields[0].receiver && !fields[1].receiver &&
               fields[0].parameter == 0 && fields[1].parameter == 1 &&
               fields[0].field == fields[1].field,
           "the permuting recursion and its caller require both formals' text "
           "field");
    for (std::size_t index = 0; index < fields.size(); ++index) {
      auto changed = result.mir;
      auto &omitted = const_cast<lang::MirFunctionInstance *>(
                          changed.findFunctionInstance(function.id))
                          ->ownerlessTextFields;
      omitted.erase(omitted.begin() + static_cast<std::ptrdiff_t>(index));
      expect(functionError(lang::verifyMirProgram(changed), function.id,
                           "omits an independently derived requirement"),
             "a field contract omitting either permuted formal is rejected");
    }
  }
  expect(contracts == 2, "the recursion and its caller carry field contracts");
  const auto dynamic = analyzeText(declarations + R"gti(
int main() {
  std::string owner{"dynamic"};
  Token a{.text = "ab", .line = 1};
  Token b{.text = owner.view(), .line = 2};
  return pick(a, b, 1).size() == 7 ? 0 : 1;
}
)gti");
  expect(!dynamic.canGenerateCode() &&
             hasDiagnosticCode(dynamic.diagnostics, "GTI-S2045"),
         "dynamic text cannot reach the formal a recursion permutes");
  expect(std::none_of(dynamic.diagnostics.begin(), dynamic.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code.starts_with("GTI-B");
                      }),
         "the permuted formal's contract is enforced at source");
}

void testRecursiveOwnerlessReturnMutations() {
  const auto result = analyzeText(R"gti(
mut std::string_view kept = "";
std::string_view pick(std::string_view s, std::string_view t, int n) {
  if (n == 0) {
    return "done";
  }
  return pick(t, s, n - 1);
}
void keep(std::string_view a, std::string_view b) { kept = pick(a, b, 2); }
std::string_view even(std::string_view s, int n) {
  return n == 0 ? "even" : odd(s, n - 1);
}
std::string_view odd(std::string_view s, int n) {
  if (n == 0) {
    return "odd";
  }
  return even(s, n - 1);
}
int main() {
  keep("x", "y");
  return kept.size() == 4 && even("x", 3).size() == 3 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "ownerless recursive results may be stored in static text");
  if (!result.canGenerateCode())
    return;
  const auto *pick = returningLiteral(result.mir, "done");
  const auto *even = returningLiteral(result.mir, "even");
  const auto *odd = returningLiteral(result.mir, "odd");
  const lang::MirFunctionInstance *keep = nullptr;
  for (const auto &function : result.mir.functionInstances())
    if (function.definitionKind == lang::MirDefinitionKind::Source &&
        function.returnType.kind == lang::SemanticType::Void &&
        function.parameterTypes.size() == 2 &&
        function.parameterTypes[0].kind == lang::SemanticType::StringView &&
        function.parameterTypes[1].kind == lang::SemanticType::StringView &&
        callsTextResult(result.mir, function.body))
      keep = &function;
  expect(pick && even && odd && keep, "the recursions and the store exist");
  if (!pick || !even || !odd || !keep)
    return;
  expect(keep->ownerlessTextParameters.empty(),
         "storing an ownerless recursive result requires no formal");
  // Returning a formal where the recursion permutes formals makes the
  // result hold either one: a store of it requires both of the caller's
  // formals, not only the one that a single unrolling reaches.
  for (const bool both : {false, true}) {
    auto changed = result.mir;
    expect(returnFirstFormal(changed, pick->id, "done"),
           "the permuting recursion returns its first formal");
    const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(keep->id))
        ->ownerlessTextParameters =
        both ? std::vector<std::size_t>{0, 1} : std::vector<std::size_t>{0};
    const auto verification = lang::verifyMirProgram(changed);
    expect(functionError(verification, pick->id,
                         "ownerless text return has no independent result "
                         "proof"),
           "a recursion returning its formal has no ownerless result proof");
    expect(functionError(verification, keep->id,
                         "ownerless text input contract differs") != both,
           "the store's derived requirement is exactly both formals");
  }
  // A formal returned in one function of a mutual recursion reaches the
  // other's result through the cycle.
  auto changed = result.mir;
  expect(returnFirstFormal(changed, odd->id, "odd"),
         "the mutual recursion returns a formal");
  const auto verification = lang::verifyMirProgram(changed);
  for (const auto *function : {even, odd})
    expect(functionError(verification, function->id,
                         "ownerless text return has no independent result "
                         "proof"),
           "a formal returned through a mutual recursion has no ownerless "
           "result proof");
}

void testRecursiveTextSummaryScaling() {
  // A summary is computed once per function, and once per pass of a set of
  // mutually recursive functions. Evaluating every call path separately
  // takes exponential time in the depth of this chain, whose returns each
  // call the next function twice, and in the size of this set, whose every
  // function calls every other.
  std::string source;
  constexpr int depth = 18;
  source += "std::string_view c" + std::to_string(depth) +
            "(std::string_view s, int n) { return \"leaf\"; }\n";
  for (int index = depth - 1; index >= 0; --index) {
    const auto next = "c" + std::to_string(index + 1);
    source += "std::string_view c" + std::to_string(index) +
              "(std::string_view s, int n) {\n  if (n == 0) { return " + next +
              "(s, n); }\n  return " + next + "(s, n - 1);\n}\n";
  }
  constexpr int members = 8;
  for (int index = 0; index < members; ++index) {
    source +=
        "std::string_view d" + std::to_string(index) +
        "(std::string_view s, int n) {\n  if (n == 0) { return \"d\"; }\n";
    for (int other = 0; other < members; ++other)
      if (other != index)
        source += "  if (n == " + std::to_string(other + 1) + ") { return d" +
                  std::to_string(other) + "(s, n - 1); }\n";
    source += "  return d" + std::to_string((index + 1) % members) +
              "(s, n - 1);\n}\n";
  }
  source += "int main() { return c0(\"x\", 1).size() + d0(\"x\", 3).size() == "
            "5 ? 0 : 1; }\n";
  const auto result = analyzeText(source);
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "a deep call chain and a set of mutually recursive functions that "
         "all call each other return ownerless text");
}

const std::string kReferenceAccessors = R"gti(
#include <std/string>
mut std::string_view kept = "";
class Token {
public:
  std::string_view text;
  int kind;
  Token(std::string_view text, int kind) : text(text), kind(kind) {}
  std::string_view& ref() { return this.text; }
  int& kindRef() { return this.kind; }
  int& kindOf() { return this.kindRef(); }
  std::string_view& alias() { return this.ref(); }
};
class Box<T> {
public:
  T value;
  Box(T value) : value(value) {}
  T& get() { return this.value; }
};
void keep(Token& token) { kept = token.ref(); }
uint64_t width(Token& token) { return token.ref().size(); }
)gti";

// Text read through a returned reference is the text that the reference
// designates: the receiver's direct text field, or an enum payload. Values
// holding ownerless text may store it.
void testTextReferenceAccessors() {
  const auto result = analyzeText(kReferenceAccessors + R"gti(
int main() {
  Token token = Token("token", 1);
  kept = token.ref();
  kept = token.alias();
  keep(token);
  Box<std::string_view> box = Box<std::string_view>("boxed");
  kept = box.get();
  expected<int, std::string_view> failed = unexpected("failure");
  kept = failed.error();
  return width(token) == 5 && token.kindOf() == 1 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "text that ownerless values return by reference may be stored");
  if (!result.canGenerateCode())
    return;
  // Storing a formal's text through its accessor requires that field, which
  // MIR derives through the returned reference by itself.
  const lang::MirFunctionInstance *keep = nullptr;
  for (const auto &function : result.mir.functionInstances())
    if (function.definitionKind == lang::MirDefinitionKind::Source &&
        function.returnType.kind == lang::SemanticType::Void &&
        function.parameterTypes.size() == 1 &&
        function.ownerlessTextFields.size() == 1)
      keep = &function;
  expect(keep != nullptr,
         "storing text through a formal's accessor requires its field");
  if (keep != nullptr) {
    auto changed = result.mir;
    const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(keep->id))
        ->ownerlessTextFields.clear();
    expect(functionError(lang::verifyMirProgram(changed), keep->id,
                         "ownerless text field contract omits an "
                         "independently derived requirement"),
           "MIR derives the field requirement through the accessor");
  }
  // A returned reference that a call borrowed from the receiver lets the
  // whole receiver escape, so `kindOf` requires each of its text fields.
  const lang::MirFunctionInstance *kindOf = nullptr;
  for (const auto &function : result.mir.functionInstances())
    if (function.definitionKind == lang::MirDefinitionKind::Source &&
        function.returnType.kind == lang::SemanticType::Reference &&
        function.returnType.arguments.size() == 1 &&
        function.returnType.arguments.front().kind ==
            lang::SemanticType::Int32 &&
        !function.ownerlessTextFields.empty())
      kindOf = &function;
  expect(kindOf != nullptr,
         "returning a reference borrowed through a call is a whole use");
  if (kindOf != nullptr) {
    auto changed = result.mir;
    const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(kindOf->id))
        ->ownerlessTextFields.clear();
    expect(functionError(lang::verifyMirProgram(changed), kindOf->id,
                         "ownerless text field contract omits an "
                         "independently derived requirement"),
           "MIR derives the whole use of a returned call reference");
  }

  // A value over a local owner reaches a reference accessor only through a
  // relay (testTextReferenceRelays), never one that lets its receiver
  // escape whole or stores what it reads. An accessor returning another
  // accessor's reference composes its relay.
  const auto composed =
      analyzeText(kReferenceAccessors +
                  "int run() { std::string input{\"input\"}; Token t = "
                  "Token(input.view(), 1); std::println(t.alias()); return 0; "
                  "}\nint main() { return run(); }\n");
  expect(composed.canGenerateCode() &&
             lang::verifyMirProgram(composed.mir).valid(),
         "a value over a local owner calls a composed reference relay");
  for (const char *body : {"kept = t.ref();", "keep(t);",
                           "std::println(t.kindOf());", "kept = t.alias();"}) {
    const auto rejected =
        analyzeText(kReferenceAccessors +
                    "int run() { std::string input{\"input\"}; Token t = "
                    "Token(input.view(), 1); " +
                    body + " return 0; }\nint main() { return run(); }\n");
    expect(hasDiagnosticCode(rejected.diagnostics, "GTI-S2045"),
           std::string("a value over a local owner reaches no escaping "
                       "accessor: ") +
               body);
    expect(std::none_of(rejected.diagnostics.begin(),
                        rejected.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           std::string("diagnosed at source: ") + body);
  }
  // A temporary receiver would need a result loan for its text reference.
  const auto temporary = analyzeText(kReferenceAccessors + R"gti(
int main() {
  kept = Token("literal", 1).ref();
  return kept.size() == 7 ? 0 : 1;
}
)gti");
  expect(hasDiagnosticCode(temporary.diagnostics, "GTI-S2081") &&
             std::none_of(temporary.diagnostics.begin(),
                          temporary.diagnostics.end(),
                          [](const auto &diagnostic) {
                            return diagnostic.code.starts_with("GTI-B");
                          }),
         "a temporary receiver of a text reference accessor is diagnosed at "
         "source");
}

// A method that returns a reference to its receiver's text field relays that
// field (returnedTextReference): on a value over a local owner its result is
// a view of the owner, as a read of the field is, and a view copied from it
// keeps the owner borrowed.
void testTextReferenceRelays() {
  const auto result = analyzeText(kReferenceAccessors + R"gti(
int main() {
  std::string input{"relayed"};
  Token t = Token(input.view(), 1);
  std::println(t.ref());
  std::string_view v = t.ref();
  std::println(v);
  return width(t) == 7 && v.size() == 7 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "a reference relay's result views the receiver's owner");
  if (!result.canGenerateCode())
    return;
  // The copied view carries the relay's result loan, whose stored
  // dependency is the owner's loan designating the relayed field. MIR derives
  // that the view holds the owner's text through the reference, so the view
  // must carry a loan that retains that publication.
  const lang::MirFunctionInstance *carrier = nullptr;
  std::size_t carried = 0;
  for (const auto &function : result.mir.functionInstances()) {
    const auto &loans = function.body.loans;
    for (std::size_t index = 0; index < loans.size(); ++index) {
      const auto &loan = loans[index];
      if (loan.kind == lang::MirLoanKind::CallResult &&
          loan.carriers.size() == 1 && loan.storedDependency != 0 &&
          loan.storedDependency <= loans.size() &&
          loans[loan.storedDependency - 1].kind == lang::MirLoanKind::Stored &&
          loans[loan.storedDependency - 1].designatedFields.size() == 1) {
        carrier = &function;
        carried = index;
      }
    }
  }
  expect(carrier != nullptr, "the copied view carries the relay's loan, "
                             "which depends on the owner's loan");
  if (carrier != nullptr) {
    const auto mutated = [&](const auto &change) {
      auto changed = result.mir;
      change(const_cast<lang::MirFunctionInstance *>(
                 changed.findFunctionInstance(carrier->id))
                 ->body.loans[carried]);
      return lang::verifyMirProgram(changed).valid();
    };
    expect(!mutated([](lang::MirLoan &loan) { loan.carriers.clear(); }),
           "a view copied through a relayed reference must carry its loan");
    expect(!mutated([](lang::MirLoan &loan) { loan.storedDependency = 0; }),
           "a relayed reference's loan must retain the owner's loan");
  }

  struct Case {
    const char *body;
    const char *code;
  };
  for (const Case &entry : {
           Case{"kept = t.ref();", "GTI-S2045"},
           Case{"keep(t);", "GTI-S2045"},
           Case{"std::string_view v = t.ref(); input.append(\"!\"); "
                "uint64_t n = v.size();",
                "GTI-S2017"},
           Case{"std::string_view& r = t.ref(); Token u = std::move(t); "
                "uint64_t n = r.size();",
                "GTI-S2017"},
       }) {
    const auto rejected = analyzeText(
        kReferenceAccessors +
        "int run() { mut std::string input{\"input\"}; Token t = "
        "Token(input.view(), 1); " +
        entry.body + " return 0; }\nint main() { return run(); }\n");
    expect(hasDiagnosticCode(rejected.diagnostics, entry.code),
           std::string("a relayed view is checked at source: ") + entry.body);
    expect(std::none_of(rejected.diagnostics.begin(),
                        rejected.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           std::string("diagnosed at source: ") + entry.body);
  }
}

void testNativeCStringProducer() {
  const auto result = analyzeText(R"gti(
extern "C" {
  c_string native_text(int32_t kind);
  void native_release(c_string text);
}
class NativeText {
  c_string text;
public:
  unsafe NativeText(c_string input) : text(input) {}
  NativeText(NativeText&) = delete;
  NativeText(NativeText&&) = default;
  ~NativeText() { native_release(this.text); }
};
void retain(int32_t kind) {
  unsafe { auto text = NativeText(native_text(kind)); }
}
int main() { retain(0); return 0; }
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "native C-string results can enter an externally managed owner");
  if (!result.canGenerateCode())
    return;
  const auto producer = std::find_if(
      result.mir.functionInstances().begin(),
      result.mir.functionInstances().end(), [](const auto &function) {
        return function.linkage == lang::LanguageLinkage::C &&
               function.returnType.kind == lang::SemanticType::CString;
      });
  expect(producer != result.mir.functionInstances().end(),
         "the regression retains its native C-string producer");
  if (producer == result.mir.functionInstances().end())
    return;
  for (int mutation = 0; mutation < 3; ++mutation) {
    auto changed = result.mir;
    auto *target = const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(producer->id));
    if (mutation == 0)
      target->linkage = lang::LanguageLinkage::Gti;
    else if (mutation == 1)
      target->definitionKind = lang::MirDefinitionKind::Source;
    else
      target->returnBorrowOrigin = lang::BorrowOriginKind::Argument;
    const auto verification = lang::verifyMirProgram(changed);
    expect(std::any_of(verification.errors.begin(), verification.errors.end(),
                       [](const auto &error) {
                         return error.message ==
                                "ownerless text storage or call lacks exact "
                                "producer evidence";
                       }),
           "native text evidence requires the exact external untracked C "
           "producer contract");
  }

  const auto dependent = analyzeText(R"gti(
#include <std/string>
class NativeText {
  c_string text;
public:
  unsafe NativeText(c_string input) : text(input) {}
  ~NativeText() {}
};
int main() {
  std::string owner{"dynamic"};
  unsafe { auto text = NativeText(owner.c_string()); }
  return 0;
}
)gti");
  expect(!dependent.canGenerateCode() &&
             std::any_of(
                 dependent.diagnostics.begin(), dependent.diagnostics.end(),
                 [](const auto &error) { return error.code == "GTI-S2045"; }) &&
             std::none_of(dependent.diagnostics.begin(),
                          dependent.diagnostics.end(),
                          [](const auto &error) {
                            return error.code.starts_with("GTI-B");
                          }),
         "GTI owner-dependent C strings still fail at source, including "
         "inside unsafe code");
}

// A payload-enum field reached through the receiver's accessor holds what
// the field does: an arm returning its text payload by reference relays it,
// and one binding it by value places the same ownerless requirement as
// `switch (this.word)` would, which MIR derives through the accessor's body.
const std::string kAccessorSubjects = R"gti(
#include <std/panic>
#include <std/string>
mut std::string_view kept = "";
enum Word { text(std::string_view value), none };
class Line {
public:
  Word word;
  Line(std::string_view value) : word(Word::text(value)) {}
  Word& wordRef() { return this.word; }
  std::string_view& viaPlace() {
    switch (this.word) {
    case Word::text(auto& v): return v;
    case Word::none: std::panic("none");
    }
  }
  std::string_view& viaCall() {
    switch (this.wordRef()) {
    case Word::text(auto& v): return v;
    case Word::none: std::panic("none");
    }
  }
  std::string_view firstCall() {
    switch (this.wordRef()) {
    case Word::text(v): return v;
    case Word::none: return "";
    }
  }
};
)gti";

void testAccessorSubjectRelays() {
  const auto result = analyzeText(kAccessorSubjects + R"gti(
int main() {
  std::string input{"relayed"};
  Line line = Line(input.view());
  std::println(line.viaCall());
  std::string_view v = line.viaCall();
  Line fixed = Line("static");
  kept = fixed.firstCall();
  return v.size() == line.viaPlace().size() ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "an accessor subject relays its payload text like the field does");
  if (!result.canGenerateCode())
    return;
  const lang::MirFunctionInstance *first = nullptr;
  for (const auto &function : result.mir.functionInstances())
    if (function.definitionKind == lang::MirDefinitionKind::Source &&
        function.returnType.kind == lang::SemanticType::StringView &&
        function.parameterTypes.empty() &&
        function.ownerlessTextFields.size() == 1 &&
        function.ownerlessTextFields.front().receiver &&
        function.ownerlessTextFields.front().payload != 0)
      first = &function;
  expect(first != nullptr, "binding a payload through the receiver's "
                           "accessor requires the field's payload ownerless");
  if (first != nullptr) {
    auto changed = result.mir;
    const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(first->id))
        ->ownerlessTextFields.clear();
    expect(functionError(lang::verifyMirProgram(changed), first->id,
                         "ownerless text field contract omits an "
                         "independently derived requirement"),
           "MIR derives the payload requirement through the accessor");
  }

  struct Case {
    const char *body;
    const char *code;
  };
  for (const Case &entry : {
           Case{"kept = line.firstCall();", "GTI-S2045"},
           Case{"kept = line.viaCall();", "GTI-S2045"},
           Case{"std::string_view v = line.viaCall(); input.append(\"!\"); "
                "uint64_t n = v.size();",
                "GTI-S2017"},
       }) {
    const auto rejected = analyzeText(
        kAccessorSubjects +
        "int run() { mut std::string input{\"input\"}; Line line = "
        "Line(input.view()); " +
        entry.body + " return 0; }\nint main() { return run(); }\n");
    expect(hasDiagnosticCode(rejected.diagnostics, entry.code),
           std::string("an accessor subject's text is checked at source: ") +
               entry.body);
    expect(std::none_of(rejected.diagnostics.begin(),
                        rejected.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           std::string("diagnosed at source: ") + entry.body);
  }
  // Returning the by-value payload of a local owner's text would dangle.
  const auto escaped = analyzeText(kAccessorSubjects + R"gti(
std::string_view leak() {
  std::string input{"freed"};
  Line line = Line(input.view());
  return line.firstCall();
}
int main() { return leak().size() == 5 ? 0 : 1; }
)gti");
  expect(hasDiagnosticCode(escaped.diagnostics, "GTI-S2045") &&
             !escaped.canGenerateCode(),
         "a by-value payload of a local owner cannot escape as ownerless");

  // Every accessor form the subject can take keeps the payload's contract:
  // a named field precisely, any other reference into the receiver as a
  // whole use. None lets the local owner's text escape, and MIR, which sees
  // through the same accessors, never finds a requirement semantics missed.
  struct Form {
    const char *accessor;
    const char *subject;
  };
  for (const Form &form : {
           Form{"Word& pick(int index) { return this.word; }", "this.pick(0)"},
           Form{"Word& wordAt() { return this.word; }", "wordAt()"},
           Form{"Word& implicitWord() { return word; }", "this.implicitWord()"},
           Form{"Word& either(bool which) { if (which) { return this.word; } "
                "return this.spare; }",
                "this.either(true)"},
       }) {
    const std::string declarations = std::string(R"gti(
#include <std/string>
enum Word { text(std::string_view value), none };
class Line {
public:
  Word word;
  Word spare;
  Line(std::string_view value) : word(Word::text(value)), spare(Word::none) {}
  )gti") + form.accessor + R"gti(
  std::string_view first() {
    switch ()gti" + form.subject + R"gti() {
    case Word::text(v): return v;
    case Word::none: return "";
    }
  }
};
)gti";
    const auto ownerless = analyzeText(declarations + R"gti(
int main() { Line line = Line("static"); return line.first().size() == 6 ? 0 : 1; }
)gti");
    if (!ownerless.canGenerateCode())
      printDiagnostics(ownerless);
    expect(ownerless.canGenerateCode() &&
               lang::verifyMirProgram(ownerless.mir).valid(),
           std::string("static text through `") + form.subject +
               "` satisfies the contract MIR derives");
    const auto rejected = analyzeText(declarations + R"gti(
std::string_view leak() {
  std::string input{"freed"};
  Line line = Line(input.view());
  return line.first();
}
int main() { return leak().size() == 5 ? 0 : 1; }
)gti");
    expect(hasDiagnosticCode(rejected.diagnostics, "GTI-S2045") &&
               !rejected.canGenerateCode(),
           std::string("a payload through `") + form.subject +
               "` cannot escape its local owner");
    expect(std::none_of(rejected.diagnostics.begin(),
                        rejected.diagnostics.end(),
                        [](const auto &diagnostic) {
                          return diagnostic.code.starts_with("GTI-B");
                        }),
           std::string("diagnosed at source: ") + form.subject);
  }
}

// Any use of an accessor's result but a switch on it reads the field it
// returns: passing it on or copying it out may carry a text payload away, so
// it requires the field's payloads ownerless, in semantics and in MIR. The
// accessor itself, whose every return borrows the field, requires nothing.
const std::string kAccessorEscapes = R"gti(
#include <std/string>
mut std::string_view kept = "";
enum Word { text(std::string_view value), none };
void keepWord(Word& w) {
  switch (w) {
  case Word::text(v): kept = v; break;
  case Word::none: break;
  }
}
class Line {
public:
  Word word;
  Line(std::string_view value) : word(Word::text(value)) {}
  Word& wordRef() { return this.word; }
  Word& pick(int index) { return this.word; }
  void passOut() { keepWord(this.wordRef()); }
  void passPick() { keepWord(this.pick(0)); }
  void copyOut() {
    Word w = this.wordRef();
    switch (w) {
    case Word::text(v): kept = v; break;
    case Word::none: break;
    }
  }
};
)gti";

void testAccessorFieldEscapes() {
  const auto literal = analyzeText(kAccessorEscapes + R"gti(
int main() {
  Line("literal").passOut();
  Line("literal").passPick();
  Line("literal").copyOut();
  return 0;
}
)gti");
  expect(literal.canGenerateCode(),
         "an accessor's result may escape over ownerless text");
  if (literal.canGenerateCode()) {
    std::size_t escapes = 0;
    for (const auto &function : literal.mir.functionInstances()) {
      if (function.definitionKind != lang::MirDefinitionKind::Source ||
          !function.owner)
        continue;
      if (function.returnType.kind == lang::SemanticType::Reference) {
        expect(function.ownerlessTextFields.empty(),
               "the accessor itself requires nothing");
        continue;
      }
      if (!function.parameterTypes.empty())
        continue;
      if (function.ownerlessTextFields.empty())
        continue;
      ++escapes;
      auto changed = literal.mir;
      const_cast<lang::MirFunctionInstance *>(
          changed.findFunctionInstance(function.id))
          ->ownerlessTextFields.clear();
      expect(functionError(lang::verifyMirProgram(changed), function.id,
                           "ownerless text field contract omits an "
                           "independently derived requirement"),
             "MIR derives an accessor escape's requirement");
    }
    expect(escapes == 3,
           "passOut, passPick and copyOut each require the payload");
  }
  for (const std::string call :
       {"line.passOut();", "line.passPick();", "line.copyOut();"}) {
    const auto rejected =
        analyzeText(kAccessorEscapes +
                    "int main() { std::string input{\"dynamic\"}; Line line = "
                    "Line(input.view()); " +
                    call + " return 0; }\n");
    expect(hasDiagnosticCode(rejected.diagnostics, "GTI-S2045") &&
               std::none_of(rejected.diagnostics.begin(),
                            rejected.diagnostics.end(),
                            [](const auto &diagnostic) {
                              return diagnostic.code.starts_with("GTI-B");
                            }),
           "an accessor escape over a local owner is rejected at source: " +
               call);
  }
}

// The source function whose qualified name is `name`.
const lang::MirFunctionInstance *
namedFunction(const lang::FrontendResult &result, std::string_view name) {
  for (const auto &function : result.mir.functionInstances()) {
    const auto *info = result.semantics.findFunction(function.declaration);
    if (function.definitionKind == lang::MirDefinitionKind::Source &&
        info != nullptr && info->qualifiedName == name)
      return &function;
  }
  return nullptr;
}

// Whether a program is rejected at source with `code` and no internal error.
bool rejectedAtSource(const lang::FrontendResult &result,
                      const std::string &code) {
  return !result.canGenerateCode() &&
         hasDiagnosticCode(result.diagnostics, code) &&
         std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code.starts_with("GTI-B");
                      });
}

// Semantics never nests dynamic text in an array (L-OWN-02), so a view copied
// out of an array holds what the array holds: ownerless text, wherever the
// array lives. MIR proves it independently: each element write, an array
// literal's included, is a checked text store, and an element read is then
// an ownerless result proof. A user-defined record's array field and a
// lambda's result follow the same rules.
const std::string kArrayTextStorage = R"gti(
#include <std/string>
struct Shelf {
  std::string_view titles[2];
  int count;
  Shelf() : titles({"x", "yz"}), count(2) {}
};
std::string_view table[3] = {"zero", "one", "two"};
mut std::string_view kept = "";
std::string_view pick(std::string_view names[3], uint64_t index) {
  return names[index];
}
std::string_view fromTable(uint64_t index) { return table[index]; }
std::string_view fromLocal(uint64_t index) {
  std::string_view local[2] = {"left", "right"};
  return local[index];
}
std::string_view fromGrid(uint64_t row, uint64_t column) {
  std::string_view grid[2][2] = {{"a", "b"}, {"c", "d"}};
  return grid[row][column];
}
std::string_view fromShelf(Shelf shelf, uint64_t index) {
  return shelf.titles[index];
}
std::string_view fromDefault(uint64_t index) {
  std::string_view empty[2] = {};
  return empty[index];
}
std::string_view place(std::string_view text, uint64_t at) {
  mut std::string_view slots[2] = {"first", "second"};
  slots[at] = text;
  return slots[at];
}
uint64_t stamp(std::string_view text, uint64_t at) {
  std::string_view copy = text;
  mut std::string_view slots[2] = {"stored", "spare"};
  slots[at] = "stamped";
  return slots[at].size() + copy.size();
}
std::string_view labelled(std::string_view prefix) {
  auto label = [](std::string_view unused) -> std::string_view {
    std::string_view copy = unused;
    return "label";
  };
  std::string_view labels[2] = {label(prefix), "other"};
  return labels[0];
}
std::string_view throughReferences() {
  mut std::string_view words[2] = {"a", "bc"};
  for (mut std::string_view& word : words) {
    word = "q";
  }
  std::string_view& last = words[1];
  return last == "q" ? words[0] : "missed";
}
uint64_t guard() {
  std::string owner{"dynamic"};
  std::string_view view = owner.view();
  std::string_view copy = view;
  mut std::string_view slots[2] = {"first", "second"};
  slots[0] = "guarded";
  return slots[0].size() + copy.size();
}
)gti";

void testArrayElementTextStorage() {
  const auto result = analyzeText(kArrayTextStorage + R"gti(
int main() {
  std::string_view names[3] = {"ab", "cde", ""};
  kept = pick(names, uint64_t(1));
  if (kept != "cde" || fromTable(uint64_t(2)) != "two") { return 1; }
  if (fromLocal(uint64_t(1)) != "right" ||
      fromGrid(uint64_t(1), uint64_t(0)) != "c") { return 2; }
  if (fromShelf(Shelf(), uint64_t(1)) != "yz" ||
      !fromDefault(uint64_t(0)).empty()) { return 3; }
  if (place("literal", uint64_t(1)) != "literal" ||
      stamp("ab", uint64_t(0)) != uint64_t(9)) { return 4; }
  if (throughReferences() != "q" || guard() != uint64_t(14)) { return 5; }
  return labelled("prefix") == "label" ? 0 : 6;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "a view read from an array element is an ownerless result");
  if (!result.canGenerateCode())
    return;
  for (const char *name :
       {"pick", "fromTable", "fromLocal", "fromGrid", "fromShelf",
        "fromDefault", "place", "labelled", "throughReferences"}) {
    const auto *function = namedFunction(result, name);
    expect(function != nullptr && textResult(*function) &&
               function->returnBorrowOrigin == lang::BorrowOriginKind::None,
           std::string("an array element return is ownerless: ") + name);
  }
  const auto *place = namedFunction(result, "place");
  const auto *stamp = namedFunction(result, "stamp");
  const auto *labelled = namedFunction(result, "labelled");
  expect(place && stamp && labelled, "the array writers exist");
  if (!place || !stamp || !labelled)
    return;
  // Storing a formal in an element requires ownerless text from each
  // caller, as storing it in a field does. MIR derived no such requirement
  // for a local array, which reported GTI-B0001 against semantics' contract.
  expect(place->ownerlessTextParameters == std::vector<std::size_t>{0},
         "an element write of a formal requires ownerless text");
  {
    auto changed = result.mir;
    const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(place->id))
        ->ownerlessTextParameters.clear();
    expect(functionError(lang::verifyMirProgram(changed), place->id,
                         "ownerless text input contract differs"),
           "MIR derives an element write's requirement");
  }
  // An element assignment and an array literal element are each a checked
  // text store: storing the formal there instead of a literal requires it.
  expect(stamp->ownerlessTextParameters.empty(),
         "literal elements require nothing");
  for (const char *literal : {"stamped", "stored"}) {
    auto changed = result.mir;
    expect(returnFirstFormal(changed, stamp->id, literal),
           std::string("the element store reads a formal: ") + literal);
    expect(functionError(lang::verifyMirProgram(changed), stamp->id,
                         "ownerless text input contract differs"),
           std::string("an array element store is checked text storage: ") +
               literal);
  }
  // A local owner's view is not ownerless text: storing it in an element
  // fails MIR verification, as storing it in a field would.
  const auto *guard = namedFunction(result, "guard");
  expect(guard != nullptr, "the guarded store exists");
  if (guard != nullptr) {
    auto changed = result.mir;
    auto *body = &const_cast<lang::MirFunctionInstance *>(
                      changed.findFunctionInstance(guard->id))
                      ->body;
    std::optional<lang::HirBindingId> view;
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions) {
        const auto *read =
            instruction.kind == lang::MirInstructionKind::Load &&
                    instruction.operands.size() == 1
                ? body->findPlace(instruction.operands.front().place)
                : nullptr;
        if (read && read->root == lang::MirPlaceRootKind::Binding &&
            read->projections.empty() &&
            read->type.kind == lang::SemanticType::StringView)
          view = read->binding;
      }
    expect(view && loadFormalForLiteral(*body, *view, "guarded"),
           "the element store reads the local owner's view");
    expect(functionError(lang::verifyMirProgram(changed), guard->id,
                         "ownerless text storage or call lacks exact producer "
                         "evidence"),
           "a local owner's view cannot fill an array element");
  }
  // A lambda's result holds what its returns hold; one returning its formal
  // has no relay contract, so its result cannot fill an array.
  auto changed = result.mir;
  bool mutated = false;
  for (const auto &lambda : changed.lambdaInstances())
    if (!lambda.parameterBindings.empty())
      mutated =
          mutated || loadFormalForLiteral(
                         const_cast<lang::MirLambdaInstance &>(lambda).body,
                         lambda.parameterBindings.front(), "label");
  expect(mutated, "the lambda returns its formal");
  expect(functionError(lang::verifyMirProgram(changed), labelled->id,
                       "ownerless text storage or call lacks exact producer "
                       "evidence"),
         "a lambda result stored in an array needs its returns' proof");
}

// Arrays still never hold a local owner's text, and a view of one still
// cannot escape: each is rejected at source, before MIR.
void testArrayElementTextRejections() {
  for (const std::string body :
       {"std::string_view names[2] = {owner.view(), \"x\"}; "
        "return names[1].size() == uint64_t(1) ? 0 : 1;",
        "mut std::string_view names[2] = {\"a\", \"x\"}; "
        "names[0] = owner.view(); return names[1].size() == uint64_t(1) ? 0 "
        ": 1;",
        "return place(owner.view(), uint64_t(0)).empty() ? 1 : 0;"}) {
    const auto rejected = analyzeText(
        kArrayTextStorage + "int main() { std::string owner{\"dynamic\"}; " +
        body + " }\n");
    expect(rejectedAtSource(rejected, "GTI-S2045"),
           "dynamic text cannot reach an array: " + body);
  }
  const auto leak = analyzeText(R"gti(
#include <std/string>
std::string_view leak() {
  std::string owner{"local"};
  std::string_view names[1] = {"static"};
  return owner.view();
}
int main() { return leak().empty() ? 1 : 0; }
)gti");
  expect(rejectedAtSource(leak, "GTI-S2045"),
         "a view of a local owner still cannot be returned");
}

// Text read through a raw pointer is an inert native value: unsafe code took
// on its validity, and raw writes are not checked text stores, so it has no
// GTI owner to relay. A C string reached through `c_string*`, a view through
// a raw pointer and a user-defined record's text field behind one each
// return ownerless text.
void testRawMemoryTextReads() {
  const auto result = analyzeText(R"gti(
#include <std/string>
struct Entry {
  std::string_view name;
  int id;
};
struct Slots {
  c_string* items = nullptr;
  int32_t count = 0;
public:
  Slots() = default;
};
mut c_string keptText = nullptr;
mut std::string_view keptView = "";
void put(c_string* items, int32_t at, c_string value) {
  unsafe { items[at] = value; }
}
c_string second(c_string* items) {
  unsafe {
    c_string* next = items + 1;
    return *next;
  }
}
c_string at(c_string* items, int32_t index) { unsafe { return items[index]; } }
c_string viaLocal(c_string* items, int32_t index) {
  unsafe {
    c_string* base = items;
    return base[index];
  }
}
c_string first(Slots slots) { unsafe { return slots.items[0]; } }
c_string viaReference(c_string*& items) { unsafe { return items[1]; } }
std::string_view viewAt(std::string_view* item) { unsafe { return *item; } }
std::string_view wordAt(std::string_view* items, int32_t index) {
  unsafe { return items[index]; }
}
std::string_view nameAt(Entry* entry) { unsafe { return entry->name; } }
int main() {
  mut c_string slots[3] = {nullptr, nullptr, nullptr};
  put(&slots[0], 1, "b");
  keptText = second(&slots[0]);
  if (keptText == nullptr || keptText != slots[1] ||
      at(&slots[0], 1) != keptText || viaLocal(&slots[0], 2) != nullptr) {
    return 1;
  }
  mut c_string* base = &slots[0];
  Slots view{.items = base, .count = 3};
  if (first(view) != nullptr || viaReference(base) != keptText) { return 2; }
  mut std::string_view words[2] = {"word", "other"};
  keptView = viewAt(&words[0]);
  mut Entry entry{.name = "entry", .id = 1};
  if (wordAt(&words[0], 1) != "other" || nameAt(&entry) != "entry") {
    return 3;
  }
  return keptView == "word" ? 0 : 4;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "text read through a raw pointer is an ownerless result");
  // Semantics and MIR agree: a raw index or `->` through a formal, a field or
  // a reference reads raw memory, not storage of that origin, and nominates
  // no relay. Semantics used to relay the pointer formal, and every call
  // reported GTI-B0001.
  if (result.canGenerateCode())
    for (const char *name : {"second", "at", "viaLocal", "first",
                             "viaReference", "viewAt", "wordAt", "nameAt"}) {
      const auto *function = namedFunction(result, name);
      const auto *info =
          function ? result.semantics.findFunction(function->declaration)
                   : nullptr;
      expect(function != nullptr && textResult(*function) &&
                 function->returnBorrowOrigin == lang::BorrowOriginKind::None &&
                 info != nullptr &&
                 info->returnBorrowOrigin == lang::BorrowOriginKind::None,
             std::string("a raw-memory text return is ownerless: ") + name);
    }
  // A GTI owner's C string is not raw memory, inside unsafe code or not.
  const auto dependent = analyzeText(R"gti(
#include <std/string>
c_string leak(c_string* items) {
  std::string owner{"local"};
  unsafe {
    c_string* next = items + 1;
    if (*next != nullptr) {
      return *next;
    }
  }
  return owner.c_string();
}
int main() { mut c_string slots[2] = {nullptr, nullptr}; return leak(&slots[0]) == nullptr ? 1 : 0; }
)gti");
  expect(rejectedAtSource(dependent, "GTI-S2045"),
         "an owner-dependent C string still cannot escape beside a raw read");
  // A raw read nominates nothing, so it neither relays a formal nor lets a
  // dynamic formal escape beside it: validity is unchanged.
  const auto either = analyzeText(R"gti(
std::string_view either(std::string_view s, std::string_view* p, bool b) {
  if (b) {
    return s;
  }
  unsafe { return p[0]; }
}
int main() { mut std::string_view v = "x"; return either("a", &v, true) == "a" ? 0 : 1; }
)gti");
  expect(rejectedAtSource(either, "GTI-S2045"),
         "a formal returned beside a raw read still needs one owner");
}

// Invoking a callable whose result is text yields what the callee returns,
// however the callee is reached. A GTI invocation pointer only comes from a
// forming site that names one stateless function or lambda, so invoking it
// yields what one of the program's targets of that pointer type returns: a
// user-defined callable class viewed through `std::function_ref`, a lambda,
// or a function item behind `std::function_ptr` all return ownerless text.
void testCallableTextResults() {
  const auto result = analyzeText(R"gti(
#include <std/functional>
class Greeter {
public:
  std::string_view greeting;
  Greeter(std::string_view greeting) : greeting(greeting) {}
  std::string_view operator()() { return this.greeting; }
};
std::string_view small(int n) { return n < 10 ? "small" : "large"; }
std::string_view label(std::string_view unused) {
  std::string_view copy = unused;
  return "label";
}
std::string_view invoke(std::function_ptr<std::string_view(int)> pick, int n) {
  return pick(n);
}
mut std::string_view kept = "";
int main() {
  Greeter hello = Greeter("hello");
  std::function_ref<std::string_view()> greet{hello};
  kept = greet();
  if (kept != "hello") { return 1; }
  auto lit = []() -> std::string_view { return "lit"; };
  std::function_ref<std::string_view()> viewed{lit};
  if (viewed() != "lit") { return 2; }
  kept = invoke(std::function_ptr<std::string_view(int)>(small), 30);
  auto zero = [](int n) -> std::string_view { return n == 0 ? "zero" : "other"; };
  std::function_ptr<std::string_view(int)> viaLambda{zero};
  std::function_ptr<std::string_view(std::string_view)> named{label};
  return kept == "large" && viaLambda(0) == "zero" && named("x") == "label"
             ? 0 : 3;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "a callable's text result through an invocation pointer is ownerless");
  if (!result.canGenerateCode())
    return;
  // Invocation methods: each returns what its pointer's targets return.
  std::vector<lang::HirFunctionInstanceId> invokers;
  for (const auto &function : result.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (textResult(function) &&
            instruction.intrinsic ==
                lang::IntrinsicKind::FunctionPointerInvoke &&
            std::find(invokers.begin(), invokers.end(), function.id) ==
                invokers.end())
          invokers.push_back(function.id);
  expect(invokers.size() >= 2,
         "both callable views invoke through a pointer and return its text");
  const auto *label = namedFunction(result, "label");
  expect(label != nullptr, "the named target exists");
  if (label == nullptr)
    return;
  // A target that returned its formal would make every invocation of its
  // pointer type return unknown text: the invocation has no relay.
  auto changed = result.mir;
  expect(returnFirstFormal(changed, label->id, "label"),
         "the target returns its formal");
  const auto verification = lang::verifyMirProgram(changed);
  expect(std::any_of(invokers.begin(), invokers.end(),
                     [&](lang::HirFunctionInstanceId invoker) {
                       return functionError(verification, invoker,
                                            "ownerless text return has no "
                                            "independent result proof");
                     }),
         "an invocation returns what its pointer's targets return");
}

// A lambda's text over a copy or move capture of a view has an owner the
// compiler cannot name (L-OWN-02): it is used locally. An expression, a local
// binding, a copy of that binding, a call that only reads or relays it,
// another lambda over the closure and a user-defined generic method that
// only reads it each keep it local, and MIR verifies the same program. Each
// keeps the captured view's owner borrowed only until its last use, so the
// owner may change afterwards. A captured literal-backed view has no owner
// and may be reassigned; a captured record's text field and a literal-backed
// lambda result are ownerless and stay storable.
void testCapturedTextLocalUse() {
  const auto result = analyzeText(R"gti(
#include <std/string>
struct Note {
  std::string_view text;
  int line;
};
class Token {
public:
  std::string_view text;
  int n;
  Token(std::string_view text, int n) : text(text), n(n) {}
};
Token shown(std::string_view text, std::string_view label) {
  std::println(label);
  return Token(text, 1);
}
class Printer {
public:
  mut int printed;
  Printer() : printed(0) {}
  void show<F>(F view) mut {
    std::string_view shown = view();
    std::println(shown);
    this.printed += 1;
  }
};
uint64_t measure(std::string_view text) { return text.size(); }
std::string_view same(std::string_view text) { return text; }
T echo<T>(T value) {
  auto copy = [value]() -> T { return value; };
  T local = copy();
  return value;
}
std::string_view keepLocal(std::string_view text) {
  auto keep = [text]() -> std::string_view { return text; };
  std::string_view held = keep();
  std::println(held);
  return "kept";
}
mut std::string_view kept = "";
int main() {
  mut std::string owner{"dynamic"};
  std::string_view view = owner.view();
  auto captured = [view]() -> std::string_view { return view; };
  std::println(captured());
  std::string_view bound = captured();
  std::string_view copied = bound;
  std::string_view relayed = same(captured());
  auto again = captured;
  auto nested = [again]() -> std::string_view { return again(); };
  std::string_view inner = [view]() -> std::string_view {
    std::string_view local = view;
    return local;
  }();
  mut Printer printer{};
  printer.show(nested);
  Token token = shown("literal", captured());
  bool used = measure(bound) == measure(copied) && relayed == inner &&
              printer.printed == token.n;
  owner.append("!");
  std::string_view fixedText = "fixed";
  auto fixed = [fixedText]() -> std::string_view { return fixedText; };
  mut std::string_view reassigned = "";
  reassigned = fixed();
  mut std::string text = std::string("native");
  c_string name = text.c_string();
  auto native = [name]() -> c_string { return name; };
  c_string got = native();
  Note note{.text = "noted", .line = 1};
  auto noted = [note]() -> std::string_view { return note.text; };
  kept = noted();
  auto literal = []() -> std::string_view { return "literal"; };
  std::string_view shelf[1] = {literal()};
  return used && reassigned == "fixed" && got != nullptr &&
                 shelf[0] == "literal" &&
                 keepLocal(owner.view()) == "kept" &&
                 echo(owner.view()) == "dynamic!"
             ? 0
             : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "a lambda's captured text may be used locally");
}

const std::string kCapturedTextEscapes = R"gti(
#include <std/string>
#include <std/functional>
struct Note {
  std::string_view text;
  int line;
};
class Token {
public:
  std::string_view text;
  int n;
  Token(std::string_view text, int n) : text(text), n(n) {}
};
class Label {
public:
  mut std::string_view text;
  Label() : text("") {}
  void set(std::string_view text) mut { this.text = text; }
};
enum class Message { text(std::string_view value), none };
mut std::string_view kept = "";
void keep(std::string_view text) { kept = text; }
std::string_view same(std::string_view text) { return text; }
Token make(std::string_view text) { return Token(text, 1); }
std::string_view call<F>(F view) { return view(); }
)gti";

// Each `marker`'s last `anchor` starts a GTI-S2045 naming `reason`.
struct CapturedTextEscape {
  std::string_view marker;
  std::string_view anchor;
  std::string_view reason;
};

void expectCapturedTextEscapes(const std::string &body,
                               std::initializer_list<CapturedTextEscape> sites,
                               std::string_view hint) {
  const std::string source = kCapturedTextEscapes + body;
  const auto result = analyzeText(source);
  expect(rejectedAtSource(result, "GTI-S2045"),
         "captured text escapes are rejected at source");
  for (const auto &site : sites) {
    const auto at = source.find(site.marker);
    expect(at != std::string::npos &&
               hasDiagnosticAt(result.diagnostics, "GTI-S2045",
                               "ownerless-text-contracts.gti",
                               at + site.marker.rfind(site.anchor),
                               site.reason),
           "captured text is rejected at " + std::string(site.marker));
  }
  expect(hasDiagnosticHint(result.diagnostics, std::string(hint)),
         "the rejection says why the text has no owner to name");
}

// The same text has no path to a return, a field, an array element, a
// payload, a global, an ownerless parameter or an invocation pointer:
// semantics reports GTI-S2045 at each site, before MIR could. A local view
// that is ever given such text holds it for its whole scope, so its escape
// is rejected even where it precedes the assignment.
void testCapturedTextEscapes() {
  const std::string_view escape =
      "cannot escape without one exact owner dependency";
  const std::string_view viewHint =
      "A lambda's view over its captured text has an owner the compiler "
      "cannot name";
  expectCapturedTextEscapes(R"gti(
std::string_view direct(std::string_view v) {
  auto f = [v]() -> std::string_view { return v; };
  return f();
}
std::string_view bound(std::string_view v) {
  auto f = [v]() -> std::string_view { return v; };
  std::string_view held = f();
  return held;
}
std::string_view looped(std::string_view v) {
  std::string_view fixed = "fixed";
  auto f = [fixed]() -> std::string_view { return fixed; };
  mut std::string_view out = "x";
  for (mut int i = 0; i < 2; i += 1) {
    if (i == 1) { return out; }
    out = f();
  }
  return "y";
}
std::string_view relayed(std::string_view v) {
  auto f = [v]() -> std::string_view { return v; };
  return same(f());
}
std::string_view nested(std::string_view v) {
  auto inner = [v]() -> std::string_view { return v; };
  auto outer = [inner]() -> std::string_view { return inner(); };
  return outer();
}
std::string_view immediate(std::string_view v) {
  return [v]() -> std::string_view { return v; }();
}
int main() { return 0; }
)gti",
                            {{"return f();", ")", escape},
                             {"return held;", "held", escape},
                             {"return out;", "out", escape},
                             {"return same(f());", ")", escape},
                             {"return outer();", ")", escape},
                             {"return v; }();", ")", escape}},
                            viewHint);
  expectCapturedTextEscapes(
      R"gti(
c_string native(c_string c) {
  auto g = [c]() -> c_string { return c; };
  return g();
}
int main() { return 0; }
)gti",
      {{"return g();", ")", "owner-dependent c_string cannot escape"}},
      "A lambda's C string over its captured text");
  expectCapturedTextEscapes(
      R"gti(
int stores(std::string_view v) {
  auto f = [v]() -> std::string_view { return v; };
  kept = f();
  mut Label label{};
  label.text = f();
  Note note{.text = f(), .line = 1};
  Token token = Token(f(), 1);
  Token made = make(f());
  std::string_view shelf[2] = {f(), "x"};
  mut std::string_view slots[1] = {"x"};
  slots[0] = f();
  Message message = Message::text(f());
  keep(f());
  label.set(f());
  return note.line + token.n + made.n;
}
int locals(std::string_view v) {
  auto f = [v]() -> std::string_view { return v; };
  std::string_view held = f();
  kept = held;
  std::string_view fixed = "fixed";
  auto g = [fixed]() -> std::string_view { return fixed; };
  mut std::string_view later = "";
  kept = later;
  later = g();
  auto store = [v]() -> int { kept = v; return 0; };
  return store();
}
class Box {
public:
  std::string_view text;
  Box(std::string_view s) : text([s]() -> std::string_view { return s; }()) {}
};
int main() { return stores("x") + locals("y"); }
)gti",
      {{"kept = f();", ")", "assigned into an existing binding"},
       {"label.text = f()", ")", "stored in a field"},
       {"{.text = f()", ")", "stored in a field"},
       {"Token(f()", ")", "passed to a constructor that stores it in a field"},
       {"make(f()", ")", "passed to a function that returns it in a field"},
       {"{f()", ")", "nested in a fixed array"},
       {"slots[0] = f()", ")", "assigned through a checked index"},
       {"Message::text(f()", ")", "stored in a payload-enum field"},
       {"keep(f()", ")", "passed to a function requiring ownerless text"},
       {"label.set(f()", ")", "passed to a function requiring ownerless text"},
       {"kept = held;", "held", "assigned into an existing binding"},
       {"kept = later;", "later", "assigned into an existing binding"},
       {"kept = v;", "v", "assigned into an existing binding"},
       {"return s; }()", ")", "stored in a constructor field"}},
      viewHint);
  // A generic function's return of what its callable returns is rejected in
  // the instance that receives such a lambda, and erasing the lambda into a
  // GTI invocation pointer would hand its text to callers of the erased
  // call: neither has an owner to relay.
  expectCapturedTextEscapes(R"gti(
int main() {
  std::string owner{"dynamic"};
  std::string_view v = owner.view();
  auto f = [v]() -> std::string_view { return v; };
  return call(f).size() == uint64_t(7) ? 0 : 1;
}
)gti",
                            {{"return view();", ")", escape}}, viewHint);
  expectCapturedTextEscapes(
      R"gti(
int main() {
  std::string owner{"dynamic"};
  std::string_view v = owner.view();
  auto f = [v]() -> std::string_view { return v; };
  std::function_ref<std::string_view()> erased{f};
  return erased().size() == uint64_t(7) ? 0 : 1;
}
)gti",
      {{"erased{f}", "{", "returned through a GTI invocation pointer"}},
      viewHint);
}

// A closure over a dynamic view, and each call result, local or copy that
// holds its captured text, keeps that view's owner borrowed for its uses, as
// the view itself does; so does a reference or callable view of the closure.
// A later change, move or replacement of the owner while one is still used
// reports GTI-S2017 at that operation, and so does one through a user-defined
// owner's field. Like the view itself, the text cannot be assigned into an
// existing binding, so it cannot outlive an inner owner through an outer one.
void testCapturedTextBorrowsOwner() {
  const std::string prelude = R"gti(
#include <std/string>
#include <std/functional>
struct Doc {
  mut std::string body;
  Doc() : body("doc-body") {}
};
std::string_view same(std::string_view text) { return text; }
int main() {
  mut std::string owner{"dynamic"};
  std::string_view view = owner.view();
  auto captured = [view]() -> std::string_view { return view; };
)gti";
  struct Case {
    std::string body;
    std::string_view marker;
    std::string_view reason;
  };
  const std::vector<Case> cases{
      {"std::string_view held = captured(); owner.append(\"!\"); "
       "std::println(held); return 0;",
       "owner.append(\"!\")",
       "while a reference borrowed from it may still be live"},
      {"std::string_view held = captured(); std::string_view copy = held; "
       "owner.append(\"!\"); std::println(copy); return 0;",
       "owner.append(\"!\")",
       "while a reference borrowed from it may still be live"},
      {"std::string_view held = same(captured()); owner.append(\"!\"); "
       "std::println(held); return 0;",
       "owner.append(\"!\")",
       "while a reference borrowed from it may still be live"},
      {"auto again = captured; owner.append(\"!\"); std::println(again()); "
       "return 0;",
       "owner.append(\"!\")",
       "while a reference borrowed from it may still be live"},
      {"auto outer = [captured]() -> std::string_view { return captured(); }; "
       "owner.append(\"!\"); std::println(outer()); return 0;",
       "owner.append(\"!\")",
       "while a reference borrowed from it may still be live"},
      {"owner.append(\"!\"); return int(captured().size());",
       "owner.append(\"!\")",
       "while a reference borrowed from it may still be live"},
      {"std::string_view held = captured(); std::string moved = "
       "std::move(owner); std::println(held); return 0;",
       "owner); std::println", "Cannot move storage"},
      {"std::string_view held = captured(); owner = std::string(\"other\"); "
       "std::println(held); return 0;",
       "owner = std::string", "Cannot assign to storage"},
      {"auto& alias = captured; owner.append(\"!\"); "
       "std::println(alias()); return 0;",
       "owner.append(\"!\")",
       "while a reference borrowed from it may still be live"},
      {"auto size = [view]() -> int { return int(view.size()); }; "
       "std::function_ref<int()> sized{size}; owner.append(\"!\"); "
       "return sized();",
       "owner.append(\"!\")",
       "while a reference borrowed from it may still be live"},
      {"mut Doc doc{}; std::string_view body = doc.body.view(); auto read = "
       "[body]() -> std::string_view { return body; }; std::string_view held "
       "= read(); doc.body.append(\"!\"); std::println(held); return 0;",
       "doc.body.append(\"!\")",
       "while a reference borrowed from it may still be live"},
  };
  for (const auto &item : cases) {
    const std::string source = prelude + "  " + item.body + "\n}\n";
    const auto result = analyzeText(source);
    const auto at = source.rfind(item.marker);
    // The owner operation the borrow conflicts with is the primary span.
    const bool reported =
        at != std::string::npos &&
        std::any_of(
            result.diagnostics.begin(), result.diagnostics.end(),
            [&](const lang::Diagnostic &diagnostic) {
              return diagnostic.code == "GTI-S2017" &&
                     diagnostic.primary.start >= at &&
                     diagnostic.primary.start < at + item.marker.size() &&
                     diagnostic.message.find(item.reason) != std::string::npos;
            });
    if (!reported)
      printDiagnostics(result);
    expect(rejectedAtSource(result, "GTI-S2017") && reported,
           "captured text keeps its owner borrowed: " + item.body);
  }
  expectCapturedTextEscapes(
      R"gti(
int sameScope(std::string_view v) {
  auto f = [v]() -> std::string_view { return v; };
  mut std::string_view later = "";
  later = f();
  return int(later.size());
}
int outerScope() {
  mut std::string_view outer = "";
  {
    std::string inner{"temporary"};
    std::string_view w = inner.view();
    auto h = [w]() -> std::string_view { return w; };
    outer = h();
  }
  return int(outer.size());
}
int main() { return sameScope("x") + outerScope(); }
)gti",
      {{"later = f()", ")", "assigned into an existing binding"},
       {"outer = h()", ")", "assigned into an existing binding"}},
      "A lambda's view over its captured text has an owner the compiler "
      "cannot name");
}

// A local text binding holds what any of its writes stored: its
// initialization, each assignment, in any order and in a loop, and a checked
// store through a mutable borrow of it (MIR TextContracts). Semantics assigns
// only ownerless text into an existing binding, so a reassigned local returns,
// stores and fills a field as ownerless text. Were an assignment to store a
// formal, a function without a relay would return unproven text.
void testReassignedTextLocals() {
  const auto result = analyzeText(R"gti(
#include <std/string>
struct Note {
  std::string_view text;
  int line;
};
void setTo(mut std::string_view& out, std::string_view value) { out = value; }
std::string_view pick(std::string_view unused, bool c) {
  std::string_view copy = unused;
  mut std::string_view v = "first";
  if (c) { v = "second"; }
  return v;
}
std::string_view looped(std::string_view unused, int n) {
  std::string_view copy = unused;
  mut std::string_view current = "start";
  mut std::string_view previous = "none";
  for (mut int i = 0; i < n; i += 1) {
    previous = current;
    current = previous;
    current = "loop";
  }
  return previous;
}
std::string_view through(std::string_view unused) {
  mut std::string_view v = "before";
  setTo(v, "after");
  return v;
}
mut std::string_view kept = "";
int main() {
  mut std::string_view w = "x";
  w = "y";
  kept = w;
  Note note{.text = w, .line = 1};
  return pick("", true) == "second" && looped("", 2) == "loop" &&
                 through("") == "after" && kept == "y" && note.text == "y"
             ? 0
             : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "a reassigned local text binding holds ownerless text");
  if (!result.canGenerateCode())
    return;
  for (const auto &[name, literal] :
       {std::pair<std::string_view, std::string_view>{"pick", "second"},
        std::pair<std::string_view, std::string_view>{"looped", "loop"}}) {
    const auto *function = namedFunction(result, name);
    expect(function != nullptr, "the function exists: " + std::string(name));
    if (function == nullptr)
      continue;
    auto changed = result.mir;
    expect(returnFirstFormal(changed, function->id, literal),
           "an assignment stores the formal");
    const auto verification = lang::verifyMirProgram(changed);
    if (!functionError(verification, function->id,
                       "ownerless text return has no independent result "
                       "proof"))
      for (const auto &error : verification.errors)
        std::cerr << error.message << '\n';
    expect(functionError(verification, function->id,
                         "ownerless text return has no independent result "
                         "proof"),
           "every write of a returned local is traced: " + std::string(name));
  }
}

// A method's relay contract leaves room for ownerless alternatives, as a
// literal is. A static call of a text function without a dependency, or of a
// text field relay whose result carries no loan, returns ownerless text,
// through the receiver's field or a nested one (MIR).
void testReceiverFieldCallReturns() {
  const auto result = analyzeText(R"gti(
#include <std/string_view>
class Inner {
public:
  int n;
  std::string_view text;
  Inner(int n) : n(n), text("inner-text") {}
  std::string_view label() { return this.n < 10 ? "small" : "large"; }
  std::string_view field() { return this.text; }
};
class Middle {
public:
  Inner inner;
  Middle(int n) : inner(Inner(n)) {}
};
class Box {
  Inner inner;
  Middle middle;
public:
  Box(int n) : inner(Inner(n)), middle(Middle(n)) {}
  std::string_view name() { return this.inner.label(); }
  std::string_view deep() { return this.middle.inner.label(); }
  std::string_view relay() { return this.inner.field(); }
};
int main() {
  Box box = Box(3);
  return box.name() == "small" && box.deep() == "small" &&
                 box.relay() == "inner-text"
             ? 0
             : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "a relay contract may return an ownerless call result");
  if (!result.canGenerateCode())
    return;
  const auto *name = namedFunction(result, "Box::name");
  const auto *label = namedFunction(result, "Inner::label");
  expect(name != nullptr && label != nullptr, "the methods exist");
  if (name == nullptr || label == nullptr)
    return;
  // Were the callee's result to depend on its receiver, the returned call
  // would need the escaping loan that relays it.
  auto changed = result.mir;
  auto *target = const_cast<lang::MirFunctionInstance *>(
      changed.findFunctionInstance(label->id));
  target->returnBorrowOrigin = lang::BorrowOriginKind::Receiver;
  const auto verification = lang::verifyMirProgram(changed);
  if (!functionError(verification, name->id,
                     "owner-dependent return is missing its escaping loan"))
    for (const auto &error : verification.errors)
      std::cerr << error.message << '\n';
  expect(functionError(verification, name->id,
                       "owner-dependent return is missing its escaping loan"),
         "only a dependency-free result is an ownerless alternative");
}

// A virtual call's text result is its root's contract, and an override keeps
// it; a lambda's result is what its returns hold; a generic method whose
// result its instance makes text relays at each call as the instance does.
// So an erased callable over a literal lambda returns ownerless text through
// a user-defined interface and through std::function, and MIR verifies it.
const std::string kErasedTextResults = R"gti(
#include <std/functional>
#include <std/polymorphic>
#include <std/string>
interface Labeler {
  std::string_view label();
};
class Fixed : public Labeler {
public:
  Fixed() {}
  std::string_view label() override { return "fixed"; }
};
class Wrapped<F> : public Labeler {
  F fn;
public:
  Wrapped(F fn) : fn(fn) {}
  std::string_view label() override { return this.fn(); }
};
class Erased {
  mut std::inline_polymorphic<Labeler, 32, 8> target{};
public:
  Erased() {}
  void set<F>(F fn) mut { this.target.replace(Wrapped<F>(fn)); }
  std::string_view operator()() { return this.target.get().label(); }
};
class Echo<R> {
public:
  int n;
  Echo() : n(0) {}
  R same(R text) { return text; }
  R operator()(R text) { return text; }
};
mut std::string_view kept = "";
)gti";

void testVirtualAndErasedTextResults() {
  const std::string valid = kErasedTextResults + R"gti(
int main() {
  auto lit = []() -> std::string_view { return "lit"; };
  mut Erased erased{};
  erased.set(std::move(lit));
  kept = erased();
  mut std::inline_polymorphic<Labeler, 32, 8> slot{};
  slot.replace(Fixed());
  std::println(slot.get().label());
  auto word = []() -> std::string_view { return "word"; };
  std::function<std::string_view()> fn{word};
  std::string_view held = fn();
  Echo<std::string_view> echo{};
  std::string owner{"echoed"};
  std::string_view relayed = echo.same(owner.view());
  std::string_view called = echo(owner.view());
  return kept == "lit" && held == "word" && relayed == called ? 0 : 1;
}
)gti";
  const auto result = analyzeText(valid);
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "virtual, lambda and generic text results lower through MIR");
  if (result.canGenerateCode()) {
    // The interface's bodyless root and the erased call's operator.
    const lang::MirFunctionInstance *root = nullptr;
    const lang::MirFunctionInstance *erased = nullptr;
    for (const auto &function : result.mir.functionInstances()) {
      const auto *info = result.semantics.findFunction(function.declaration);
      if (info == nullptr)
        continue;
      if (info->qualifiedName == "Labeler::label")
        root = &function;
      if (function.definitionKind == lang::MirDefinitionKind::Source &&
          info->qualifiedName == "Erased::__gti_operator_call")
        erased = &function;
    }
    expect(root != nullptr && erased != nullptr,
           "the interface root and the erased call exist");
    if (root != nullptr && erased != nullptr) {
      // Were the root to relay its receiver, the erased call's result would
      // be the overrides' receivers' text, which no proof reaches.
      auto changed = result.mir;
      auto *target = const_cast<lang::MirFunctionInstance *>(
          changed.findFunctionInstance(root->id));
      target->returnBorrowOrigin = lang::BorrowOriginKind::Receiver;
      const auto verification = lang::verifyMirProgram(changed);
      if (!functionError(verification, erased->id,
                         "ownerless text return has no independent result "
                         "proof"))
        for (const auto &error : verification.errors)
          std::cerr << error.message << '\n';
      expect(functionError(verification, erased->id,
                           "ownerless text return has no independent result "
                           "proof"),
             "a virtual call is ownerless only under a dependency-free root");
    }
  }
  // An override keeps its root's contract: a view of the override's own
  // string is not ownerless text.
  const std::string owned = R"gti(
#include <std/polymorphic>
#include <std/string>
interface Named {
  std::string_view name();
};
class Owner : public Named {
  std::string text;
public:
  Owner() : text("owned") {}
  std::string_view name() override { return this.text.view(); }
};
int main() {
  mut std::inline_polymorphic<Named, 64, 8> slot{};
  slot.replace(Owner());
  return int(slot.get().name().size());
}
)gti";
  const auto rejected = analyzeText(owned);
  const auto at = owned.find("this.text.view()");
  expect(
      rejectedAtSource(rejected, "GTI-S2045") && at != std::string::npos &&
          hasDiagnosticAt(rejected.diagnostics, "GTI-S2045",
                          "ownerless-text-contracts.gti",
                          at + std::string_view("this.text.view()").size() - 1,
                          "cannot escape without one exact owner "
                          "dependency"),
      "an override of a dependency-free root returns ownerless text");
  // A generic result its instance makes text relays at the call, so the
  // relayed owner stays borrowed while the result is used.
  const std::string borrowed = kErasedTextResults + R"gti(
int main() {
  Echo<std::string_view> echo{};
  mut std::string owner{"echoed"};
  std::string_view relayed = echo.same(owner.view());
  owner.append("!");
  std::println(relayed);
  return 0;
}
)gti";
  const auto relayResult = analyzeText(borrowed);
  expect(rejectedAtSource(relayResult, "GTI-S2017"),
         "a generic text relay keeps its argument's owner borrowed");
}

// An override's text result keeps its root's contract in the override's own
// instance of the root's class: under a generic root that relays its
// argument, each override relays that argument or returns ownerless text,
// and each call keeps the argument's owner borrowed (L-OWN-02).
const std::string kGenericVirtualRoot = R"gti(
#include <std/string>
class Picker<T> {
public:
  int n;
  Picker() : n(0) {}
  virtual T pick(T a) { return a; }
};
class Literal : public Picker<std::string_view> {
public:
  Literal() : Picker<std::string_view>() {}
  std::string_view pick(std::string_view a) override { return "literal"; }
};
class Same<T> : public Picker<T> {
public:
  Same() : Picker<T>() {}
  T pick(T a) override { return a; }
};
)gti";

void testGenericVirtualRootInstances() {
  const std::string valid = kGenericVirtualRoot + R"gti(
int main() {
  std::string owner{"owned"};
  Literal literal{};
  Same<std::string_view> same{};
  Picker<std::string_view>& first = literal;
  Picker<std::string_view>& second = same;
  return first.pick(owner.view()) == "literal" &&
                 second.pick(owner.view()) == "owned"
             ? 0
             : 1;
}
)gti";
  const auto result = analyzeText(valid);
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "overrides of a generic relaying root lower through MIR");
  if (result.canGenerateCode()) {
    // The root's instance and each override relay the argument, as the
    // dispatch table requires.
    std::size_t picks = 0;
    for (const auto &function : result.mir.functionInstances()) {
      const auto *info = result.semantics.findFunction(function.declaration);
      if (info == nullptr ||
          function.returnType.kind != lang::SemanticType::StringView ||
          !info->qualifiedName.ends_with("::pick"))
        continue;
      ++picks;
      expect(function.returnBorrowOrigin == lang::BorrowOriginKind::Argument &&
                 function.returnBorrowParameter == 0,
             "each pick relays its argument: " + info->qualifiedName);
    }
    expect(picks == 3, "the root and both overrides are instantiated");
  }
  // Whichever override dispatch selects, even one returning a literal, the
  // argument's owner stays borrowed while the result is used.
  const std::string mutated = kGenericVirtualRoot + R"gti(
int main() {
  mut std::string owner{"owned"};
  Literal literal{};
  Picker<std::string_view>& picker = literal;
  std::string_view picked = picker.pick(owner.view());
  owner.append("!");
  return int(picked.size());
}
)gti";
  expect(rejectedAtSource(analyzeText(mutated), "GTI-S2017"),
         "a virtual relay keeps its argument's owner borrowed");
  // An override cannot relay what its root does not: its own string.
  const std::string receiver = kGenericVirtualRoot + R"gti(
class Owned : public Picker<std::string_view> {
  std::string text;
public:
  Owned() : Picker<std::string_view>(), text("own") {}
  std::string_view pick(std::string_view a) override {
    return this.text.view();
  }
};
int main() {
  Owned owned{};
  return int(owned.pick("x").size());
}
)gti";
  const auto rejected = analyzeText(receiver);
  const auto at = receiver.find("this.text.view()");
  expect(
      rejectedAtSource(rejected, "GTI-S2045") && at != std::string::npos &&
          hasDiagnosticAt(rejected.diagnostics, "GTI-S2045",
                          "ownerless-text-contracts.gti",
                          at + std::string_view("this.text.view()").size() - 1,
                          "must derive from read-only parameter 'a'"),
      "an override relays only what its root's instance relays");
}

// An overloaded operator's text result relays its receiver or an argument
// as its summary says, as a method's does (L-OWN-02): the relayed owner stays
// borrowed while the result is used, and an argument holding ownerless text
// relays nothing.
const std::string kOperatorTextRelays = R"gti(
#include <std/string>
class Same {
public:
  Same() {}
  std::string_view operator()(std::string_view s) { return s; }
  std::string_view operator[](std::string_view s) { return s; }
};
class Plain {
  mut std::string s;
public:
  Plain() : s("plain") {}
  std::string_view operator()() { return this.s.view(); }
  void grow() mut { this.s.append("!"); }
};
class Echo<R> {
public:
  int n;
  Echo() : n(0) {}
  R operator()(R text) { return text; }
};
)gti";

void testOperatorTextRelays() {
  const auto valid = analyzeText(kOperatorTextRelays + R"gti(
int main() {
  Same same{};
  mut Plain plain{};
  Echo<std::string_view> echo{};
  mut std::string owner{"owned"};
  bool relayed = same(owner.view()) == same[owner.view()] &&
                 echo(owner.view()) == "owned";
  std::string_view literal = same("literal");
  owner.append("!");
  bool held = plain() == "plain";
  plain.grow();
  return relayed && held && literal == "literal" ? 0 : 1;
}
)gti");
  if (!valid.canGenerateCode())
    printDiagnostics(valid);
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "operator text relays lower through MIR");
  for (const auto &[use, mutation] :
       {std::pair<std::string_view, std::string_view>{
            "std::string_view kept = same(owner.view());",
            "owner.append(\"!\");"},
        std::pair<std::string_view, std::string_view>{
            "std::string_view kept = same[owner.view()];",
            "owner.append(\"!\");"},
        std::pair<std::string_view, std::string_view>{
            "std::string_view kept = echo(owner.view());",
            "owner.append(\"!\");"},
        std::pair<std::string_view, std::string_view>{
            "std::string_view kept = plain();", "plain.grow();"}}) {
    const std::string source = kOperatorTextRelays + R"gti(
int main() {
  Same same{};
  mut Plain plain{};
  Echo<std::string_view> echo{};
  mut std::string owner{"owned"};
  )gti" + std::string(use) + "\n  " +
                               std::string(mutation) + R"gti(
  return int(kept.size());
}
)gti";
    expect(rejectedAtSource(analyzeText(source), "GTI-S2017"),
           "an operator's text result keeps its relayed owner borrowed: " +
               std::string(use));
  }
}

// A call relaying ownerless text may keep a loan on its argument while its
// result carries no borrowed state, because the relayed text has no owner.
// MIR accepts that shape whether the result is used within its full
// expression or bound to a local, as llvm_place_execution's
// nested-text-return case uses it.
void testOwnerlessTextRelayLoans() {
  const auto result = analyzeText(R"gti(
#include <std/string_view>
std::string_view relay(std::string_view value) { return value; }
int inspect(std::string_view value) {
  if (value.size() != uint64_t(4) || value[1] != '\0' || value[3] != 'c') { return 1; }
  return 0;
}
int main() {
  std::string_view text = "a\0bc";
  std::string_view kept = relay(text);
  return inspect(relay(text)) + inspect(kept);
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  const auto verification = lang::verifyMirProgram(result.mir);
  if (result.canGenerateCode() && !verification.valid())
    for (const auto &error : verification.errors)
      std::cerr << error.message << '\n';
  expect(result.canGenerateCode() && verification.valid(),
         "a relay of ownerless text verifies without borrowed state");
}

// A text call result kept under a loan on owner-dependent text carries
// borrowed state: an owner's view, a local view of it, a formal relayed in a
// function, and an owner's c_string. MIR verification derives that
// from the loan source's ownerless provenance, so clearing the borrowed state
// of any such result fails with this check, while a relay of ownerless text
// keeps its conservative loan without borrowed state.
void testDependentTextCallBorrowedState() {
  const auto result = analyzeText(R"gti(
#include <std/string>
std::string_view relay(std::string_view value) { return value; }
c_string crelay(c_string value) { return value; }
std::string_view forward(std::string_view s) { return relay(s); }
int main() {
  std::string owner{"owned"};
  std::string_view view = owner.view();
  std::string_view direct = relay(owner.view());
  std::string_view copied = relay(view);
  std::string_view forwarded = forward(owner.view());
  c_string native = crelay(owner.c_string());
  std::string_view text = "lit";
  std::string_view ownerless = relay(text);
  return direct.size() + copied.size() + forwarded.size() +
                     ownerless.size() ==
                 uint64_t(18) && native != nullptr
             ? 0
             : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "dependent and ownerless text relays verify");
  if (!result.canGenerateCode())
    return;
  const std::string message = "text call result keeps a loan on "
                              "owner-dependent text but carries no borrowed "
                              "state";
  std::size_t dependent = 0;
  std::size_t ownerless = 0;
  const auto text = [](const lang::SemanticType &type) {
    return type.kind == lang::SemanticType::StringView ||
           type.kind == lang::SemanticType::CString;
  };
  // Clears the borrowed state of one text call result kept under a loan.
  const auto check = [&](lang::MirProgram &changed, lang::MirBody &body,
                         const lang::MirInstruction &call) {
    for (auto &block : body.blocks)
      for (auto &stage : block.instructions)
        if (stage.id == call.id)
          stage.info.traits.containsBorrowedState = false;
    for (auto &value : body.values)
      if (value.id == *call.result)
        value.info.traits.containsBorrowedState = false;
    const auto verification = lang::verifyMirProgram(changed);
    return std::any_of(verification.errors.begin(), verification.errors.end(),
                       [&](const auto &error) {
                         return error.message == message &&
                                error.instruction == call.id;
                       });
  };
  const auto visit = [&](const lang::MirBody &body, const auto &locate) {
    for (const auto &block : body.blocks)
      for (const auto &call : block.instructions) {
        if (call.tombstone || call.kind != lang::MirInstructionKind::Call ||
            !call.result || !call.loan || !text(call.info.type))
          continue;
        if (!call.info.traits.containsBorrowedState) {
          ++ownerless;
          continue;
        }
        ++dependent;
        auto changed = result.mir;
        expect(check(changed, locate(changed), call),
               "a dependent text call result without borrowed state fails "
               "the text call borrowed-state check");
      }
  };
  for (const auto &function : result.mir.functionInstances())
    visit(function.body, [&](lang::MirProgram &changed) -> lang::MirBody & {
      return const_cast<lang::MirFunctionInstance *>(
                 changed.findFunctionInstance(function.id))
          ->body;
    });
  for (std::size_t index = 0; index < result.mir.lambdaInstances().size();
       ++index)
    visit(result.mir.lambdaInstances()[index].body,
          [&](lang::MirProgram &changed) -> lang::MirBody & {
            return const_cast<lang::MirLambdaInstance &>(
                       changed.lambdaInstances()[index])
                .body;
          });
  // main's string_view and c_string relays, forward's formal relay, and the
  // owner views and c_string feeding them.
  expect(dependent >= 6, "every dependent text relay was mutated");
  expect(ownerless >= 1,
         "the ownerless relay keeps its loan without borrowed state");
}

// A by-value text relay of a reference formal's text field (L-OWN-02) is a
// text field result, as returning the field directly is: the relay's result
// loan names that field place, not the formal's whole referent, so the field
// contract tracks one field and the function declares no ownerless
// requirement on it. Dependent views relayed through the same call keep their
// loans: escaping one reports GTI-S2045 and mutating its owner GTI-S2017.
void testFormalTextFieldRelays() {
  const std::string declarations = R"gti(
#include <std/string>
std::string_view relay(std::string_view value) { return value; }
class Label {
public:
  std::string_view text;
  Label() : text("label") {}
  Label(std::string_view text) : text(text) {}
};
std::string_view field_relay(Label& label) { return relay(label.text); }
)gti";
  const auto valid = analyzeText(declarations + R"gti(
int main() {
  std::string owner{"dynamic"};
  Label held{owner.view()};
  Label fixed{};
  return field_relay(held).size() + field_relay(fixed).size() == uint64_t(12)
             ? 0
             : 1;
}
)gti");
  if (!valid.canGenerateCode())
    printDiagnostics(valid);
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "a relayed formal text field verifies as a text field result");
  bool fieldLoan = false;
  bool required = false;
  for (const auto &function : valid.mir.functionInstances()) {
    required = required || !function.ownerlessTextFields.empty();
    const auto &body = function.body;
    for (const auto &block : body.blocks)
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call || !call.loan ||
            call.info.type.kind != lang::SemanticType::StringView)
          continue;
        const auto *loan = body.findLoan(*call.loan);
        const auto *source = loan ? body.findPlace(loan->source) : nullptr;
        fieldLoan =
            fieldLoan ||
            (loan && loan->kind == lang::MirLoanKind::Stored && source &&
             source->root == lang::MirPlaceRootKind::Binding &&
             source->type.kind == lang::SemanticType::StringView &&
             !source->projections.empty() &&
             source->projections.back().kind == lang::MirProjectionKind::Field);
      }
  }
  expect(fieldLoan, "the relay's result loan names the formal's text field");
  expect(!required, "a relayed text field adds no ownerless field requirement");

  const auto escaped = analyzeText(declarations + R"gti(
std::string_view leak() {
  std::string owner{"dynamic"};
  return relay(owner.view());
}
int main() { return int(leak().size()); }
)gti");
  expect(rejectedAtSource(escaped, "GTI-S2045"),
         "a dependent view relayed out of its owner's scope is rejected");
  const auto escapedField = analyzeText(declarations + R"gti(
std::string_view leak() {
  std::string owner{"dynamic"};
  Label label{owner.view()};
  return field_relay(label);
}
int main() { return int(leak().size()); }
)gti");
  expect(rejectedAtSource(escapedField, "GTI-S2045"),
         "a relayed field holding a local owner's view cannot escape");
  const auto mutated = analyzeText(declarations + R"gti(
int main() {
  mut std::string owner{"dynamic"};
  Label label{owner.view()};
  std::string_view kept = field_relay(label);
  owner.append("!");
  return int(kept.size());
}
)gti");
  expect(rejectedAtSource(mutated, "GTI-S2017"),
         "a relayed field's owner stays borrowed while its view lives");

  // A by-value formal is a local copy whose cleanup leaves the caller's text
  // alone: its relayed field returns no loan into that copy, and the caller
  // still keeps the owner borrowed.
  const std::string byValue =
      declarations +
      "std::string_view by_value(Label label) { return relay(label.text); }\n";
  const auto copied = analyzeText(byValue + R"gti(
int main() {
  std::string owner{"dynamic"};
  std::string_view kept = by_value(Label(owner.view()));
  return kept.size() == uint64_t(7) && by_value(Label()).size() == uint64_t(5)
             ? 0
             : 1;
}
)gti");
  if (!copied.canGenerateCode())
    printDiagnostics(copied);
  expect(copied.canGenerateCode() && lang::verifyMirProgram(copied.mir).valid(),
         "a relayed by-value formal text field verifies");
  expect(rejectedAtSource(analyzeText(byValue + R"gti(
std::string_view leak() {
  std::string owner{"dynamic"};
  return by_value(Label(owner.view()));
}
int main() { return int(leak().size()); }
)gti"),
                          "GTI-S2045"),
         "a by-value relay of a local owner's view cannot escape");
  expect(rejectedAtSource(analyzeText(byValue + R"gti(
int main() {
  mut std::string owner{"dynamic"};
  std::string_view kept = by_value(Label(owner.view()));
  owner.append("!");
  return int(kept.size());
}
)gti"),
                          "GTI-S2017"),
         "a by-value relay keeps its owner borrowed while its view lives");
}

// An invocation pointer's text result is ownerless (owner decision
// 2026-10-27): its signature names no origin for text, so forming one from a
// target whose text result depends on a parameter reports GTI-S2045 where it
// is formed, naming that parameter. That holds for a returned parameter, a
// view of a referenced owner, a referenced record's text field and a
// c_string. A target returning ownerless text stays accepted, and its results
// keep no argument's owner borrowed.
void testInvocationPointerTextTargets() {
  const auto accepted = analyzeText(R"gti(
#include <std/functional>
#include <std/string>
std::string_view tag(std::string_view s) {
  if (s.empty()) { return "empty"; }
  return "full";
}
int main() {
  auto quiet = [](std::string_view s) -> std::string_view {
    return s.empty() ? "none" : "some";
  };
  std::function_ptr<std::string_view(std::string_view)> named{tag};
  std::function_ptr<std::string_view(std::string_view)> lambda{quiet};
  mut std::string owner{"owned"};
  std::string_view first = named(owner.view());
  std::string_view second = lambda(owner.view());
  owner.append("!");
  return first == "full" && second == "some" ? 0 : 1;
}
)gti");
  if (!accepted.canGenerateCode())
    printDiagnostics(accepted);
  expect(accepted.canGenerateCode() &&
             lang::verifyMirProgram(accepted.mir).valid(),
         "an invocation pointer over an ownerless text target is accepted");
  struct Case {
    std::string_view declarations;
    std::string_view signature;
    std::string_view parameter;
    std::string_view result;
  };
  for (const Case &target :
       {Case{"std::string_view target(std::string_view s) {\n"
             "  if (s.empty()) { return \"empty\"; }\n"
             "  return s;\n"
             "}\n",
             "std::string_view(std::string_view)", "s", "std::string_view"},
        Case{"std::string_view target(std::string& owner) {\n"
             "  return owner.view();\n"
             "}\n",
             "std::string_view(std::string&)", "owner", "std::string_view"},
        Case{"struct Label { std::string_view text = \"label\";  public: Label() = default; };\n"
             "std::string_view target(Label& label) { return label.text; "
             "}\n",
             "std::string_view(Label&)", "label", "std::string_view"},
        Case{"c_string target(c_string native) { return native; }\n",
             "c_string(c_string)", "native", "c_string"}}) {
    const std::string source =
        "#include <std/functional>\n#include <std/string>\n" +
        std::string(target.declarations) +
        "int main() {\n  std::function_ptr<" + std::string(target.signature) +
        "> pointer{target};\n  return 0;\n}\n";
    const auto rejected = analyzeText(source);
    const auto formed = source.find("pointer{target}");
    const auto formal = source.find(" " + std::string(target.parameter) + ")");
    const std::string reason =
        "A GTI invocation pointer's " + std::string(target.result) +
        " result is ownerless, but this target returns text from its "
        "parameter '" +
        std::string(target.parameter) + "'";
    const auto *diagnostic =
        findDiagnosticByCode(rejected.diagnostics, "GTI-S2045");
    expect(
        rejectedAtSource(rejected, "GTI-S2045") &&
            formed != std::string::npos &&
            hasDiagnosticAt(rejected.diagnostics, "GTI-S2045",
                            "ownerless-text-contracts.gti",
                            formed + std::string_view("pointer").size(),
                            reason) &&
            diagnostic != nullptr && formal != std::string::npos &&
            std::any_of(diagnostic->related.begin(), diagnostic->related.end(),
                        [&](const auto &related) {
                          return related.span.start == formal + 1 &&
                                 related.message.find(
                                     "relays this parameter's text") !=
                                     std::string::npos;
                        }) &&
            hasDiagnosticHint(rejected.diagnostics, "returns ownerless text"),
        "a text-relaying invocation pointer target is rejected where it is "
        "formed: " +
            std::string(target.signature));
  }
}

// A view field a base class declares is the derived value's field too. A
// class with a base keeps its text fields ownerless, so a derived method,
// virtual or not, returns an inherited view field as ownerless text, as a
// root's own method returns its field (L-OWN-02). A derived constructor that
// passes a formal to a base constructor storing it therefore requires
// ownerless text there, at every construction and through a function that
// forwards to it; MIR derives that requirement independently.
const std::string kInheritedTextFields = R"gti(
#include <std/string>
class Base {
public:
  std::string_view text;
  Base(std::string_view text) : text(text) {}
  virtual std::string_view name() { return "base"; }
};
class Derived : public Base {
public:
  Derived(std::string_view text) : Base(text) {}
  std::string_view name() override { return this.text; }
  std::string_view plain() { return this.text; }
};
class Holder<T> {
public:
  std::string_view text;
  T value;
  Holder(std::string_view text, T value) : text(text), value(value) {}
  virtual std::string_view name() { return this.text; }
};
class Special<T> : public Holder<T> {
public:
  Special(T value) : Holder<T>("special", value) {}
  std::string_view name() override { return this.text; }
};
Derived make(std::string_view text) { return Derived(text); }
)gti";

void testInheritedTextFields() {
  const auto valid = analyzeText(kInheritedTextFields + R"gti(
int main() {
  Derived derived = make("derived");
  Base& base = derived;
  Special<int> special{1};
  Holder<int>& holder = special;
  return base.name() == "derived" && derived.plain() == "derived" &&
                 holder.name() == "special"
             ? 0
             : 1;
}
)gti");
  if (!valid.canGenerateCode())
    printDiagnostics(valid);
  expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
         "derived methods return inherited view fields as ownerless text");
  // MIR derives the factory's requirement from the base constructor its
  // derived constructor forwards to: without the declared contract, the
  // derived requirement no longer matches.
  if (valid.canGenerateCode())
    if (const auto *factory = namedFunction(valid, "make")) {
      auto changed = valid.mir;
      auto *function = const_cast<lang::MirFunctionInstance *>(
          changed.findFunctionInstance(factory->id));
      expect(function->ownerlessTextParameters == std::vector<std::size_t>{0},
             "the factory requires ownerless text for the forwarded formal");
      function->ownerlessTextParameters.clear();
      const auto verification = lang::verifyMirProgram(changed);
      expect(functionError(verification, factory->id,
                           "ownerless text input contract differs from "
                           "independently derived storage and forwarding"),
             "MIR derives a base initializer's ownerless text requirement");
    }
  // Dynamic text cannot reach an inherited field through a base constructor.
  const std::string direct = kInheritedTextFields + R"gti(
int main() {
  std::string owner{"dynamic"};
  Derived derived{owner.view()};
  return int(derived.plain().size());
}
)gti";
  const auto rejected = analyzeText(direct);
  const auto argument = direct.find("derived{owner.view()}");
  const auto base = direct.find("Base(text)");
  const auto *diagnostic =
      findDiagnosticByCode(rejected.diagnostics, "GTI-S2045");
  expect(rejectedAtSource(rejected, "GTI-S2045") &&
             argument != std::string::npos && base != std::string::npos &&
             hasDiagnosticAt(
                 rejected.diagnostics, "GTI-S2045",
                 "ownerless-text-contracts.gti",
                 argument + std::string_view("derived{owner.view()").size() - 1,
                 "passed to a constructor that stores it in a field") &&
             diagnostic != nullptr &&
             std::any_of(diagnostic->related.begin(), diagnostic->related.end(),
                         [&](const auto &related) {
                           return related.span.start == base &&
                                  related.message.find(
                                      "base initializer passes the view") !=
                                      std::string::npos;
                         }) &&
             hasDiagnosticHint(rejected.diagnostics,
                               "a base class or virtual methods keeps its "
                               "text fields ownerless"),
         "a derived constructor's base initializer requires ownerless text");
  const auto forwarded = analyzeText(kInheritedTextFields + R"gti(
int main() {
  std::string owner{"dynamic"};
  Derived derived = make(owner.view());
  return int(derived.plain().size());
}
)gti");
  expect(rejectedAtSource(forwarded, "GTI-S2045") &&
             std::any_of(forwarded.diagnostics.begin(),
                         forwarded.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.message.find(
                                      "requiring ownerless text input") !=
                                  std::string::npos;
                         }),
         "a function forwarding to that constructor requires ownerless text");
}

constexpr NamedTest kTests[] = {
    {"testArrayElementTextStorage", testArrayElementTextStorage},
    {"testArrayElementTextRejections", testArrayElementTextRejections},
    {"testRawMemoryTextReads", testRawMemoryTextReads},
    {"testCallableTextResults", testCallableTextResults},
    {"testCapturedTextLocalUse", testCapturedTextLocalUse},
    {"testCapturedTextEscapes", testCapturedTextEscapes},
    {"testCapturedTextBorrowsOwner", testCapturedTextBorrowsOwner},
    {"testReassignedTextLocals", testReassignedTextLocals},
    {"testReceiverFieldCallReturns", testReceiverFieldCallReturns},
    {"testVirtualAndErasedTextResults", testVirtualAndErasedTextResults},
    {"testGenericVirtualRootInstances", testGenericVirtualRootInstances},
    {"testOperatorTextRelays", testOperatorTextRelays},
    {"testOwnerlessTextRelayLoans", testOwnerlessTextRelayLoans},
    {"testDependentTextCallBorrowedState", testDependentTextCallBorrowedState},
    {"testFormalTextFieldRelays", testFormalTextFieldRelays},
    {"testInvocationPointerTextTargets", testInvocationPointerTextTargets},
    {"testInheritedTextFields", testInheritedTextFields},
    {"testOwnerlessForwarding", testOwnerlessForwarding},
    {"testAccessorSubjectRelays", testAccessorSubjectRelays},
    {"testAccessorFieldEscapes", testAccessorFieldEscapes},
    {"testTextReferenceAccessors", testTextReferenceAccessors},
    {"testTextReferenceRelays", testTextReferenceRelays},
    {"testNativeCStringProducer", testNativeCStringProducer},
    {"testOwnerlessTextAliasWrites", testOwnerlessTextAliasWrites},
    {"testRecursiveOwnerlessReturns", testRecursiveOwnerlessReturns},
    {"testPermutingRecursiveFieldContracts",
     testPermutingRecursiveFieldContracts},
    {"testRecursiveOwnerlessReturnMutations",
     testRecursiveOwnerlessReturnMutations},
    {"testRecursiveTextSummaryScaling", testRecursiveTextSummaryScaling},
};
} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, kTests, "ownerless text contracts");
}
