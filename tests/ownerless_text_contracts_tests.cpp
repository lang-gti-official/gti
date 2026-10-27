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

constexpr NamedTest kTests[] = {
    {"testArrayElementTextStorage", testArrayElementTextStorage},
    {"testArrayElementTextRejections", testArrayElementTextRejections},
    {"testRawMemoryTextReads", testRawMemoryTextReads},
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
