#include "../src/compiler/mir_borrow_audits.h"
#include "../src/compiler/mir_carrier_flow.h"
#include "../src/compiler/mir_storage_effects.h"
#include "../src/compiler/mir_text_contracts.h"
#include "../src/compiler/mir_verification_queries.h"
#include "compiler_test_fixture.h"
#include "gti/hir.h"
#include "gti/mir_editor.h"
#include "gti/mir_verifier.h"

namespace {
void printDiagnostics(const lang::FrontendResult &result) {
  for (const auto &diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    if (diagnostic.code != "GTI-B0001")
      continue;
    for (const auto &function : result.hir.functionInstances()) {
      const auto marker =
          "body kind " +
          std::to_string(static_cast<int>(lang::MirBodyKind::Function)) +
          ", owner " + std::to_string(function.id) + ",";
      if (diagnostic.message.find(marker) == std::string::npos)
        continue;
      const auto *info = result.semantics.findFunction(function.declaration);
      std::cerr << "FAILED FUNCTION " << function.id << ' '
                << (info ? info->qualifiedName : "<unknown>") << '\n';
      if (const auto *lowered = result.mir.findFunctionInstance(function.id))
        std::cerr << lang::MirPrinter().print(lowered->body);
    }
  }
}
lang::FrontendResult analyzeText(std::string source,
                                 bool reachableOnly = false) {
  lang::FrontendOptions options;
  options.lowerReachableOnly = reachableOnly;
  options.lowerLibraryReachableOnly = reachableOnly;
  if (reachableOnly)
    options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
  return lang::Frontend(options).analyze(
      "ownerless-text-contracts.gti", std::move(source),
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
}

void testOwnerlessForwarding() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.utility;
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
import std.string;
import std.utility;
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
import std.string;
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
import std.string;
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
import std.string;
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
import std.panic;
import std.string;
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
import std.string;
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
import std.string;
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
import std.string;
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
  for (const char *name : {"fromTable", "fromLocal", "fromGrid", "fromDefault",
                           "place", "labelled", "throughReferences"}) {
    const auto *function = namedFunction(result, name);
    expect(function != nullptr && textResult(*function) &&
               function->returnBorrowOrigin == lang::BorrowOriginKind::None,
           std::string("an array element return is ownerless: ") + name);
  }
  const auto *pick = namedFunction(result, "pick");
  expect(pick && pick->returnBorrowOrigin == lang::BorrowOriginKind::Argument &&
             pick->returnBorrowParameter == 0 &&
             pick->returnBorrowAccess == lang::AccessMode::ReadOnly,
         "an indexed formal array infers its external-content supplier");
  const auto *shelf = namedFunction(result, "fromShelf");
  expect(shelf && textResult(*shelf) &&
             shelf->returnBorrowOrigin == lang::BorrowOriginKind::Argument &&
             shelf->returnBorrowFields.size() == 1 &&
             shelf->returnBorrowFields.front().parameter == 0 &&
             shelf->returnBorrowFields.front().sourceFields.size() == 1,
         "a formal's inline array field exports its stored-text relation even "
         "when this caller supplies literals");
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
  // Indexed assignment still requires ownerless text. A local initializer
  // may instead retain the formal's dependency until the array ends.
  expect(stamp->ownerlessTextParameters.empty(),
         "literal elements require nothing");
  for (const char *literal : {"stamped", "stored"}) {
    auto changed = result.mir;
    expect(returnFirstFormal(changed, stamp->id, literal),
           std::string("the element store reads a formal: ") + literal);
    const auto proof = lang::verifyMirProgram(changed);
    const bool accepted =
        std::string_view(literal) == "stored"
            ? proof.valid()
            : functionError(proof, stamp->id,
                            "ownerless text input contract differs");
    if (!accepted)
      for (const auto &error : proof.errors)
        std::cerr << "ARRAY MUTATION " << literal << " owner=" << error.owner
                  << " " << error.message << '\n';
    expect(accepted,
           std::string("array initializer versus indexed-write obligation: ") +
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
    const auto proof = lang::verifyMirProgram(changed);
    const bool rejected =
        functionError(proof, guard->id,
                      "borrowed text cannot satisfy an ownerless storage or "
                      "call requirement");
    if (!rejected)
      for (const auto &error : proof.errors)
        std::cerr << "ARRAY GUARDED MUTATION owner=" << error.owner << " "
                  << error.message << '\n';
    expect(rejected,
           "an indexed write cannot discard the local view's dependency");
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
  const auto proof = lang::verifyMirProgram(changed);
  const bool rejected = functionError(
      proof, labelled->id,
      "borrowed text cannot satisfy an ownerless storage or call requirement");
  if (!rejected)
    for (const auto &error : proof.errors)
      std::cerr << "ARRAY LAMBDA MUTATION owner=" << error.owner << " "
                << error.message << '\n';
  expect(rejected,
         "an array cannot erase an uncontracted lambda result's dependency");
}

// Untracked indexed writes and escaping local owners retain their separate
// source restrictions; borrowed array literals have positive coverage above.
void testArrayElementTextRejections() {
  for (const std::string body :
       {"mut std::string_view names[2] = {\"a\", \"x\"}; "
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
import std.string;
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
// return ownerless text, whether the pointer is a formal, a field or a local
// binding of either.
void testRawMemoryTextReads() {
  const auto result = analyzeText(R"gti(
import std.string;
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
c_string viaAuto(c_string* items) {
  unsafe {
    auto base = items;
    return base[1];
  }
}
c_string viaMutable(c_string* items) {
  unsafe {
    mut c_string* base = items;
    base = base + 1;
    return base[0];
  }
}
c_string viaLocalRecord(Slots slots) {
  unsafe {
    Slots copy = slots;
    return copy.items[0];
  }
}
std::string_view viaLocalArrow(Entry* entry) {
  unsafe {
    Entry* local = entry;
    return local->name;
  }
}
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
  keptText = viaAuto(&slots[0]);
  if (keptText != viaMutable(&slots[0]) || first(view) != viaLocalRecord(view)) {
    return 5;
  }
  keptView = viaLocalArrow(&entry);
  return keptView == "entry" ? 0 : 4;
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
    for (const char *name :
         {"second", "at", "viaLocal", "first", "viaReference", "viewAt",
          "wordAt", "nameAt", "viaAuto", "viaMutable", "viaLocalRecord",
          "viaLocalArrow"}) {
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
import std.string;
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
import std.functional;
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
import std.string;
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
import std.string;
import std.functional;
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
// Retaining method calls are checked independently by
// testCapturedRetainingStoreEscape: they no longer require ownerless inputs,
// and their MIR proof must run without earlier semantic errors masking it.
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
// owner's field. Captured text still cannot be assigned into an existing
// binding, so it cannot outlive an inner owner through an outer one.
void testCapturedTextBorrowsOwner() {
  const std::string prelude = R"gti(
import std.string;
import std.functional;
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
        std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                    [&](const lang::Diagnostic &diagnostic) {
                      // The MIR borrow checker states the conflict generically.
                      return diagnostic.code == "GTI-S2017" &&
                             diagnostic.primary.start >= at &&
                             diagnostic.primary.start <
                                 at + item.marker.size() &&
                             (diagnostic.message.find(item.reason) !=
                                  std::string::npos ||
                              diagnostic.message.find(
                                  "overlapping borrow may still be live") !=
                                  std::string::npos);
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
// store through a mutable borrow of it (MIR TextContracts). All writes in
// this control are ownerless, so the local may return, store and fill a field
// as ownerless text. Were an assignment to store a formal, a function without
// a relay would return unproven text.
void testReassignedTextLocals() {
  const auto result = analyzeText(R"gti(
import std.string;
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
import std.string_view;
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
import std.functional;
import std.polymorphic;
import std.string;
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
import std.polymorphic;
import std.string;
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
import std.string;
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
import std.string;
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
import std.string_view;
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
import std.string;
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
import std.string;
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
import std.functional;
import std.string;
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
        Case{"struct Label { std::string_view text = \"label\";  public: "
             "Label() = default; };\n"
             "std::string_view target(Label& label) { return label.text; "
             "}\n",
             "std::string_view(Label&)", "label", "std::string_view"},
        Case{"c_string target(c_string native) { return native; }\n",
             "c_string(c_string)", "native", "c_string"}}) {
    const std::string source = "import std.functional;\nimport std.string;\n" +
                               std::string(target.declarations) +
                               "int main() {\n  std::function_ptr<" +
                               std::string(target.signature) +
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
import std.string;
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

void testMultipleTextOwners() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.utility;
struct Labels<lifetime a, lifetime b> {
  std::string_view<a> first;
  std::string_view<b> second;
  Labels(std::string_view<a> x, std::string_view<b> y):first(x),second(y){}
};
int main() {
  std::string first{"first"}; std::string second{"second"};
  Labels labels{first.view(), second.view()};
  auto copy = labels;
  auto moved = std::move(labels);
  return copy.first == "first" && copy.second == "second" &&
         moved.first == "first" && moved.second == "second" ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "a shared aggregate retains two text owners");
  if (!result.canGenerateCode())
    return;
  bool tested = false;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (!instruction.dependentTextPublication ||
            instruction.borrowFields.size() != 2 || !instruction.loan)
          continue;
        tested = true;
        for (int mutation = 0; mutation < 6; ++mutation) {
          auto changed = result.mir;
          auto &body = const_cast<lang::MirFunctionInstance *>(
                           changed.findFunctionInstance(function.id))
                           ->body;
          auto &instructions = body.findBlock(block.id)->instructions;
          auto *altered = &*std::find_if(
              instructions.begin(), instructions.end(),
              [&](const auto &item) { return item.id == instruction.id; });
          auto *loan =
              const_cast<lang::MirLoan *>(body.findLoan(*instruction.loan));
          if (mutation == 0)
            altered->borrowFields.pop_back();
          else if (mutation == 1)
            altered->borrowFields.back().parameter =
                altered->borrowFields.front().parameter;
          else if (mutation == 2)
            altered->borrowFields.back().resultFields =
                altered->borrowFields.front().resultFields;
          else if (mutation == 3)
            loan->additionalDependencies.clear();
          else if (mutation == 4) {
            expect(!loan->additionalDependencies.empty(),
                   "second owner is retained");
            if (loan->additionalDependencies.empty())
              continue;
            loan->additionalDependencies.front().source = loan->source;
          } else {
            expect(!loan->additionalDependencies.empty(),
                   "second field is retained");
            if (loan->additionalDependencies.empty())
              continue;
            loan->additionalDependencies.front().designatedFields =
                loan->designatedFields;
          }
          expect(!lang::verifyMirProgram(changed).valid(),
                 "every published field and its actual owner must be verified");
        }
      }
    }
  }
  expect(tested,
         "two-owner publication is represented by a verified loan group");
}

void testCarrierFlowOnLoweredProgram() {
  const auto result = analyzeText(R"gti(
int main() {
  mut int owner = 4;
  int& view = owner;
  int independent = view;
  owner = 5;
  return independent - 4;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "a scalar copy releases its input borrow before a write");
  if (!result.canGenerateCode())
    return;
  const auto *main = namedFunction(result, "main");
  expect(main != nullptr && !main->body.loans.empty(),
         "the source regression contains a real loan");
  if (!main)
    return;
  const auto flow = lang::deriveMirCarrierFlow(main->body);
  expect(flow.complete,
         "ordinary lowered reference/copy/store flow is complete");
  expect(std::any_of(flow.reads.begin(), flow.reads.end(),
                     [](const auto &item) {
                       return item.second && !item.second->empty();
                     }),
         "lowered uses are associated with their actual loan groups");
}

void testReplacementDependencyVerification() {
  const auto result = analyzeText(R"gti(
import std.string;
int main() {
  std::string owner{"text"};
  mut std::string_view view = owner.view();
  view = "literal";
  return int(view.size());
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(), "ownerless replacement is a valid control");
  if (!result.canGenerateCode())
    return;
  const auto *main = namedFunction(result, "main");
  expect(main != nullptr, "replacement control has a lowered body");
  if (!main)
    return;
  lang::MirLoanId dependency = 0;
  lang::MirOperand borrowed;
  for (const auto &block : main->body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.loan && instruction.result &&
          instruction.info.type.kind == lang::SemanticType::StringView) {
        dependency = *instruction.loan;
        borrowed = {.kind = lang::MirOperandKind::Value,
                    .value = *instruction.result,
                    .type = instruction.info.type};
      }
  expect(dependency != 0, "the control has an external view dependency");
  if (!dependency)
    return;
  auto changed = main->body;
  bool replaced = false;
  for (auto &block : changed.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Assign &&
          instruction.operands.size() == 1 && instruction.destination &&
          changed.findPlace(*instruction.destination)->type.kind ==
              lang::SemanticType::StringView) {
        instruction.operands.front() = borrowed;
        replaced = true;
      }
  expect(replaced, "the mutation replaces the actual view store's input");
  if (!replaced)
    return;
  // Move the dependency's endpoint to the edited store. The valid control
  // may already retire it before assigning the literal; leaving that endpoint
  // would test a duplicate EndBorrow instead of the forged value reuse.
  for (auto &block : changed.blocks)
    std::erase_if(block.instructions, [&](const auto &instruction) {
      return instruction.kind == lang::MirInstructionKind::EndBorrow &&
             instruction.loan == dependency;
    });
  lang::MirInstructionId next = 1;
  for (const auto &block : changed.blocks)
    for (const auto &instruction : block.instructions)
      next = std::max(next, instruction.id + 1);
  bool endedBeforeStore = false;
  for (auto &block : changed.blocks) {
    const auto store = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [&](const auto &instruction) {
          return instruction.kind == lang::MirInstructionKind::Assign &&
                 instruction.operands.size() == 1 &&
                 instruction.operands.front().kind ==
                     lang::MirOperandKind::Value &&
                 instruction.operands.front().value == borrowed.value;
        });
    if (store == block.instructions.end())
      continue;
    block.instructions.insert(store,
                              {.id = next++,
                               .kind = lang::MirInstructionKind::EndBorrow,
                               .loan = dependency});
    endedBeforeStore = true;
  }
  expect(endedBeforeStore,
         "the mutation explicitly ends its dependency before the store");
  if (!endedBeforeStore)
    return;
  expect(lang::rebuildMirValueUses(changed),
         "the edited store has coherent operand-use indexing");
  // Reusing its SSA value must not resurrect it, even if the carrier-list
  // annotation is erased. Inspect the specific rejection, not just failure.
  const_cast<lang::MirLoan *>(changed.findLoan(dependency))->carriers.clear();
  // Reconstruct the source-defined string constructor and accessor effects;
  // a body-only query intentionally treats those retained contents as unknown.
  const auto checked = lang::verifyMirBody(changed, main->id, &result.mir);
  const bool rejected = std::any_of(
      checked.errors.begin(), checked.errors.end(), [](const auto &error) {
        return error.borrow.kind ==
                   lang::MirBorrowRejectionKind::ExpiredDependency &&
               error.message.find(
                   "stored value uses unavailable dependency loan") !=
                   std::string::npos;
      });
  if (!rejected) {
    const auto *record = changed.findLoan(dependency);
    std::cerr << "replacement dependency " << dependency
              << " kind=" << int(record->kind) << " parent=" << record->parent
              << " stored=" << record->storedDependency
              << " entry=" << record->entry << " escapes=" << record->escapes
              << " lifecycle=" << record->lifecycleBound << '\n';
    for (const auto &error : checked.errors)
      std::cerr << "verification: " << error.message << '\n';
    const auto flow = lang::deriveMirCarrierFlow(changed, &result.mir);
    for (const auto &block : changed.blocks)
      for (const auto &instruction : block.instructions) {
        const auto reads = flow.reads.find(instruction.id);
        if ((instruction.loan && *instruction.loan == dependency) ||
            (reads != flow.reads.end() && reads->second &&
             std::find(reads->second->begin(), reads->second->end(),
                       dependency) != reads->second->end()))
          std::cerr << "dependency site block=" << block.id
                    << " instruction=" << instruction.id
                    << " kind=" << int(instruction.kind) << '\n';
      }
  }
  expect(rejected, "actual stored-value flow rejects a retired dependency "
                   "without carrier metadata");
}

void testExplicitEmptyCarrierProof() {
  const auto result = analyzeText(R"gti(
enum Reading<lifetime a> { empty, value(int&<a> item) };
Reading<static> empty_reading() {
  auto first = Reading<static>::empty;
  auto copied = first;
  return copied;
}
int main() {
  auto reading = empty_reading();
  return 0;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "a copied explicitly empty alternative remains publishable without a "
         "loan");
}

void testJoinedCarrierCannotSelectOneOrigin() {
  using namespace lang;
  MirBody body;
  body.places = {{.id = 1,
                  .root = MirPlaceRootKind::Binding,
                  .binding = 1,
                  .type = SemanticType::Int32},
                 {.id = 2,
                  .root = MirPlaceRootKind::Binding,
                  .binding = 2,
                  .type = SemanticType::Int32},
                 {.id = 3,
                  .root = MirPlaceRootKind::Binding,
                  .binding = 3,
                  .projections = {{.kind = MirProjectionKind::Dereference}},
                  .type = SemanticType::Int32}};
  body.loans = {{.id = 1, .kind = MirLoanKind::Local, .source = 1},
                {.id = 2, .kind = MirLoanKind::Local, .source = 2}};
  expect(mir_detail::loanProtectsPlace(body, 1, 3, {{3, {1}}}),
         "an exact carried reference resolves to its own protected origin");
  expect(!mir_detail::loanProtectsPlace(body, 2, 3, {{3, {1}}}),
         "a different owner cannot authorize the exact carried reference");
  expect(!mir_detail::loanProtectsPlace(body, 1, 3, {{3, {1, 2}}}) &&
             !mir_detail::loanProtectsPlace(body, 2, 3, {{3, {1, 2}}}),
         "neither alternative alone proves a joined reference's exact origin");
  expect(!mir_detail::loanProtectsPlace(body, 1, 3, {}),
         "missing carrier authority cannot prove an external origin");
}

void testArrayResultLifetimeSuppliers() {
  // Inspect signatures before MIR: suppliers must not depend on a body
  // returning literals, array rank, or a particular text representation.
  struct Case {
    std::string source;
    std::vector<std::size_t> suppliers;
  };
  const std::vector<Case> cases = {
      {R"gti(std::string_view<a>[2] paired<lifetime a>(std::string_view<a> first,
          std::string_view<a> second) { return {"first", "second"}; })gti",
       {0, 1}},
      {R"gti(std::string_view<a>[2][1] paired<lifetime a>(std::string_view<a>[1] first,
          std::string_view<a>[1] second) { return {{"first"}, {"second"}}; })gti",
       {0, 1}},
      {R"gti(c_string<a>[2] paired<lifetime a>(c_string<a> first,
          c_string<a> second) { return {nullptr, nullptr}; })gti",
       {0, 1}},
      {R"gti(std::string_view<a> paired<lifetime a>(std::string_view<a>[2] first,
          std::string_view<a>[2] second) { return "literal"; })gti",
       {0, 1}},
      {R"gti(std::string_view<a>[2] paired<lifetime a, lifetime b>(std::string_view<a> first,
          std::string_view<b> second) { return {"first", "second"}; })gti",
       {0}}};
  for (const auto &test : cases) {
    const auto result =
        lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics})
            .analyze("array-result-lifetime-suppliers.gti",
                     test.source + "\nint main() { return 0; }",
                     {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    if (!result.semanticValid)
      printDiagnostics(result);
    expect(result.semanticValid,
           "the array supplier signature is valid syntax and semantics");
    bool found = false;
    for (const auto &statement : result.program.declarations()) {
      const auto *syntax = lang::stmtAs<lang::FunctionDecl>(statement.get());
      if (!syntax || syntax->name().lexeme != "paired")
        continue;
      const auto *function = result.semantics.findFunction(*syntax);
      found = function != nullptr;
      expect(function && function->returnBorrowOrigin ==
                             lang::BorrowOriginKind::Argument,
             "an explicit array result region preserves an argument contract");
      if (function) {
        const auto suppliers =
            function->returnBorrowParameters.empty()
                ? std::vector<std::size_t>{function->returnBorrowParameter}
                : function->returnBorrowParameters;
        expect(suppliers == test.suppliers,
               "an array result retains exactly its declared region suppliers "
               "independently of its body");
      }
    }
    expect(found, "the array supplier declaration was inspected");
  }
}

void testArrayResultLiteralReturnProof() {
  const auto result = analyzeText(R"gti(
std::string_view<a>[2] paired<lifetime a>(std::string_view<a> first,
                                        std::string_view<a> second) {
  return {"first", "second"};
}
int main() { auto values = paired("one", "two"); return values[0] == "first" ? 0 : 1; }
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(), "a declared array lifetime contract permits "
                                   "an actually ownerless result");
  if (!result.canGenerateCode())
    return;
  const auto *function = namedFunction(result, "paired");
  expect(function != nullptr, "the array-return function reaches MIR");
  if (!function)
    return;
  auto changed = result.mir;
  auto *forged = const_cast<lang::MirFunctionInstance *>(
      changed.findFunctionInstance(function->id));
  forged->returnBorrowParameters = {0, 0};
  expect(!lang::verifyMirProgram(changed).valid(),
         "duplicate array-return suppliers are not a valid return contract");
}

void testInlineArrayCallSupplierSet() {
  for (const auto &arguments : {std::string{"first.view(), second.view()"},
                                std::string{"\"first\", second.view()"},
                                std::string{"first.view(), \"second\""}}) {
    const auto result = analyzeText(R"gti(
import std.string;
std::string_view<a>[2] paired<lifetime a>(std::string_view<a> first,
                                        std::string_view<a> second) {
  return {"first", "second"};
}
int main() {
  std::string first{"first"}; std::string second{"second"};
  auto values = paired()gti" + arguments +
                                    R"gti();
  return values[0] == "first" ? 0 : 1;
}
)gti");
    if (!result.canGenerateCode())
      printDiagnostics(result);
    expect(result.canGenerateCode(),
           "array return contracts retain actual suppliers including an "
           "ownerless primary");
    if (!result.canGenerateCode())
      continue;
    const auto *callee = namedFunction(result, "paired");
    const auto *caller = namedFunction(result, "main");
    expect(callee && caller, "the supplier-set call reaches MIR");
    if (!callee || !caller)
      continue;
    bool inspected = false;
    for (const auto &block : caller->body.blocks)
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call ||
            call.functionTarget != callee->id || !call.loan)
          continue;
        inspected = true;
        auto changed = result.mir;
        auto &body = const_cast<lang::MirFunctionInstance *>(
                         changed.findFunctionInstance(caller->id))
                         ->body;
        auto *loan = const_cast<lang::MirLoan *>(body.findLoan(*call.loan));
        expect(loan != nullptr,
               "borrowed actuals produce a result dependency group");
        if (!loan)
          continue;
        if (arguments == "first.view(), second.view()") {
          expect(!loan->additionalDependencies.empty(),
                 "two actual owners survive the declared return contract");
          loan->additionalDependencies.clear();
          expect(!lang::verifyMirProgram(changed).valid(),
                 "the verifier rejects a result group that drops the secondary "
                 "owner");
        }
      }
    expect(inspected, "the declared call publishes its actual owner set");
  }
}

void testReferenceArrayExternalEntry() {
  const auto result = analyzeText(R"gti(
std::string_view<a> select_view<lifetime a, lifetime shell>(
    std::string_view<a>[2]&<shell> values, int index) {
  return values[index];
}
int main() { return 0; }
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(), "a reference-array formal separates element "
                                   "owners from physical storage");
  if (!result.canGenerateCode())
    return;
  const auto *function = namedFunction(result, "select_view");
  expect(function != nullptr, "the reference-array function reaches MIR");
  if (!function)
    return;
  bool inspected = false;
  for (const auto &entry : function->body.loans) {
    if (!entry.entry || entry.kind != lang::MirLoanKind::Parameter ||
        !entry.storedDependency)
      continue;
    inspected = true;
    auto changed = result.mir;
    auto &body = const_cast<lang::MirFunctionInstance *>(
                     changed.findFunctionInstance(function->id))
                     ->body;
    auto *external =
        const_cast<lang::MirLoan *>(body.findLoan(entry.storedDependency));
    expect(external != nullptr,
           "the array reference has an external dependency witness");
    if (!external)
      continue;
    external->access = lang::AccessMode::Mutable;
    expect(!lang::verifyMirProgram(changed).valid(),
           "an array of shared views cannot manufacture mutable external "
           "authority");
  }
  expect(inspected,
         "the reference-array formal has a distinct data dependency");
}

void testExpiredStoredReturn() {
  using namespace lang;
  MirBody body;
  body.kind = MirBodyKind::Function;
  body.entry = 1;
  body.places = {{.id = 1,
                  .root = MirPlaceRootKind::Binding,
                  .binding = 1,
                  .type = SemanticType::Int32,
                  .access = AccessMode::Mutable,
                  .initiallyAvailable = true},
                 {.id = 2,
                  .root = MirPlaceRootKind::Binding,
                  .binding = 2,
                  .type = SemanticType::StringView,
                  .access = AccessMode::Mutable,
                  .traits = {.containsBorrowedState = true}}};
  body.loans = {{.id = 1, .kind = MirLoanKind::Stored, .source = 1}};
  body.blocks = {
      {.id = 1,
       .instructions =
           {{.id = 1,
             .kind = MirInstructionKind::Borrow,
             .operands = {{.kind = MirOperandKind::BorrowRead, .place = 1}},
             .loan = 1},
            {.id = 2,
             .kind = MirInstructionKind::Initialize,
             .destination = 2,
             .operands = {{.kind = MirOperandKind::Loan, .loan = 1}}},
            {.id = 3, .kind = MirInstructionKind::EndBorrow, .loan = 1}},
       .terminator = {.kind = MirTerminatorKind::Return,
                      .value = MirOperand{.kind = MirOperandKind::Copy,
                                          .place = 2,
                                          .type = SemanticType::StringView}}}};
  const auto verify = [](const MirBody &candidate) {
    return verifyMirPersistentStorageLoanFlow(candidate, 0, {});
  };
  auto discarded = body;
  discarded.blocks[0].terminator.value.reset();
  expect(
      verify(discarded).valid(),
      "an initialized borrowed carrier may be discarded after its loan ends");
  const auto expired = verify(body);
  expect(std::any_of(expired.errors.begin(), expired.errors.end(),
                     [](const auto &error) {
                       return error.borrow.kind ==
                                  MirBorrowRejectionKind::Escape &&
                              error.borrow.loan == 1;
                     }),
         "returning an expired initialized carrier is an escape without any "
         "replacement store");
  auto ownerless = body;
  ownerless.blocks[0].instructions[1].operands = {
      {.kind = MirOperandKind::Constant, .type = SemanticType::StringView}};
  expect(
      verify(ownerless).valid(),
      "an unrelated ended loan does not prevent returning an ownerless value");
}

void testConditionalStoredValueFlow() {
  // Exercise the canonical loan-flow engine independently of source admission
  // and lowering's binding state. Source acceptance is tested separately;
  // these mutations must reject invalid MIR regardless of its producer.
  using namespace lang;
  MirBody body;
  body.kind = MirBodyKind::Function;
  body.entry = 1;
  body.places = {{.id = 1,
                  .root = MirPlaceRootKind::Binding,
                  .binding = 1,
                  .type = SemanticType::Int32,
                  .access = AccessMode::Mutable,
                  .initiallyAvailable = true},
                 {.id = 2,
                  .root = MirPlaceRootKind::Binding,
                  .binding = 2,
                  .type = SemanticType::StringView,
                  .access = AccessMode::Mutable,
                  .traits = {.containsBorrowedState = true},
                  .initiallyAvailable = true}};
  body.loans = {
      {.id = 1, .kind = MirLoanKind::Stored, .source = 1, .carriers = {2}}};
  body.blocks = {
      {.id = 1,
       .instructions = {{.id = 1,
                         .kind = MirInstructionKind::Initialize,
                         .destination = 2,
                         .operands = {{.kind = MirOperandKind::Constant}}}},
       .terminator = {.kind = MirTerminatorKind::Branch,
                      .target = 2,
                      .elseTarget = 3}},
      {.id = 2,
       .instructions =
           {{.id = 2,
             .kind = MirInstructionKind::Borrow,
             .operands = {{.kind = MirOperandKind::BorrowRead, .place = 1}},
             .loan = 1},
            {.id = 3,
             .kind = MirInstructionKind::Assign,
             .destination = 2,
             .operands = {{.kind = MirOperandKind::Loan, .loan = 1}}}},
       .terminator = {.kind = MirTerminatorKind::Goto, .target = 4}},
      {.id = 3, .terminator = {.kind = MirTerminatorKind::Goto, .target = 4}},
      {.id = 4,
       .instructions =
           {{.id = 4,
             .kind = MirInstructionKind::Load,
             .operands = {{.kind = MirOperandKind::Copy, .place = 2}}},
            {.id = 5, .kind = MirInstructionKind::EndBorrow, .loan = 1}},
       .terminator = {.kind = MirTerminatorKind::Return}}};
  const auto verify = [](const MirBody &candidate) {
    return verifyMirPersistentStorageLoanFlow(candidate, 0, {});
  };
  const auto checked = verify(body);
  for (const auto &error : checked.errors)
    std::cerr << "conditional stored control: " << error.message << '\n';
  expect(checked.valid(), "a valid carried value supports ordinary reads after "
                          "a conditional store");
  auto twoOwners = body;
  auto otherOwner = twoOwners.places.front();
  otherOwner.id = 3;
  otherOwner.binding = 3;
  twoOwners.places.push_back(otherOwner);
  twoOwners.loans.push_back(
      {.id = 2, .kind = MirLoanKind::Stored, .source = 3, .carriers = {2}});
  twoOwners.blocks[2].instructions = {
      {.id = 6,
       .kind = MirInstructionKind::Borrow,
       .operands = {{.kind = MirOperandKind::BorrowRead, .place = 3}},
       .loan = 2},
      {.id = 7,
       .kind = MirInstructionKind::Assign,
       .destination = 2,
       .operands = {{.kind = MirOperandKind::Loan, .loan = 2}}}};
  twoOwners.blocks[3].instructions.push_back(
      {.id = 8, .kind = MirInstructionKind::EndBorrow, .loan = 2});
  expect(verify(twoOwners).valid(),
         "a branch join preserves a value supplied by either of two owners");
  for (const MirPlaceId owner : {MirPlaceId{1}, MirPlaceId{3}}) {
    auto invalid = twoOwners;
    invalid.blocks[3].instructions.insert(
        invalid.blocks[3].instructions.begin(),
        {.id = 9,
         .kind = MirInstructionKind::Assign,
         .destination = owner,
         .operands = {{.kind = MirOperandKind::Constant}}});
    expect(!verify(invalid).valid(),
           "each potential owner remains protected after a two-owner join");
  }
  for (auto &loan : twoOwners.loans)
    loan.carriers.clear();
  expect(verify(twoOwners).valid(),
         "two-owner join proof is reconstructed without carrier annotations");
  auto loop = body;
  loop.blocks[1].terminator = {
      .kind = MirTerminatorKind::Branch, .target = 2, .elseTarget = 4};
  const auto repeated = verify(loop);
  for (const auto &error : repeated.errors)
    std::cerr << "repeated shared store: " << error.message << '\n';
  expect(repeated.valid(),
         "one shared stored producer may execute repeatedly on a backedge");
  auto duplicated = loop;
  auto secondProducer = duplicated.blocks[1].instructions.front();
  secondProducer.id = 6;
  duplicated.blocks[1].instructions.insert(
      duplicated.blocks[1].instructions.begin() + 1, secondProducer);
  expect(!verify(duplicated).valid(),
         "distinct producers cannot reuse a live stored group identifier");
  auto expiredCopy = loop;
  auto copySlot = expiredCopy.places[1];
  copySlot.id = 3;
  copySlot.binding = 3;
  expiredCopy.places.push_back(copySlot);
  expiredCopy.blocks[0].instructions.push_back(
      {.id = 6,
       .kind = MirInstructionKind::Initialize,
       .destination = 3,
       .operands = {{.kind = MirOperandKind::Constant}}});
  expiredCopy.blocks[1].instructions.insert(
      expiredCopy.blocks[1].instructions.begin() + 1,
      {.id = 7,
       .kind = MirInstructionKind::Load,
       .operands = {{.kind = MirOperandKind::Copy, .place = 3}}});
  expiredCopy.blocks[1].instructions.push_back(
      {.id = 8,
       .kind = MirInstructionKind::Assign,
       .destination = 3,
       .operands = {{.kind = MirOperandKind::Copy, .place = 2}}});
  expiredCopy.blocks[1].instructions.push_back(
      {.id = 9, .kind = MirInstructionKind::EndBorrow, .loan = 1});
  expiredCopy.blocks[3].instructions.clear();
  const auto expired = verify(expiredCopy);
  expect(
      std::any_of(
          expired.errors.begin(), expired.errors.end(),
          [](const auto &error) {
            return error.borrow.kind ==
                       lang::MirBorrowRejectionKind::ExpiredDependency &&
                   error.borrow.loan == 1 &&
                   error.message.find(
                       "stored value uses unavailable dependency loan 1") !=
                       std::string::npos;
          }),
      "a new iteration cannot revive a copy expired on the previous iteration");
  auto exclusiveLoop = loop;
  exclusiveLoop.loans[0].access = AccessMode::Mutable;
  exclusiveLoop.blocks[1].instructions[0].operands[0].kind =
      MirOperandKind::BorrowWrite;
  expect(!verify(exclusiveLoop).valid(),
         "repeated shared production does not grant exclusive reactivation");
  auto changed = body;
  changed.loans[0].carriers.clear();
  expect(verify(changed).valid(),
         "actual stored-value proof does not depend on carrier annotations");
  changed = body;
  changed.loans.push_back({.id = 2,
                           .storedDependency = 1,
                           .kind = MirLoanKind::CallResult,
                           .source = 2});
  changed.blocks[3].instructions[0].kind = MirInstructionKind::Call;
  changed.blocks[3].instructions[0].loan = 2;
  changed.blocks[3].instructions.insert(
      changed.blocks[3].instructions.begin() + 1,
      {.id = 6, .kind = MirInstructionKind::EndBorrow, .loan = 2});
  const auto publication = verify(changed);
  expect(std::any_of(publication.errors.begin(), publication.errors.end(),
                     [](const auto &error) {
                       return error.message.find(
                                  "requires active stored dependency loan 1") !=
                              std::string::npos;
                     }),
         "ordinary value proof does not authorize incomplete borrowed-result "
         "publication");
  changed = body;
  changed.blocks[3].instructions[0].operands = {
      {.kind = MirOperandKind::Loan, .loan = 1}};
  expect(!verify(changed).valid(),
         "a naked loan ID is not created on the ownerless predecessor");
  changed = body;
  std::swap(changed.blocks[3].instructions[0],
            changed.blocks[3].instructions[1]);
  expect(
      !verify(changed).valid(),
      "ending the conditional dependency before its carried read is rejected");
  changed = body;
  changed.blocks[3].instructions.erase(changed.blocks[3].instructions.begin() +
                                       1);
  expect(!verify(changed).valid(),
         "conditional dependencies still require an endpoint before body exit");
  changed = body;
  changed.blocks[3].instructions.insert(
      changed.blocks[3].instructions.begin(),
      {.id = 6,
       .kind = MirInstructionKind::Assign,
       .destination = 1,
       .operands = {{.kind = MirOperandKind::Constant}}});
  expect(!verify(changed).valid(),
         "a conditionally retained owner is protected against mutation");
  changed = body;
  changed.loans[0].access = AccessMode::Mutable;
  changed.blocks[1].instructions[0].operands[0].kind =
      MirOperandKind::BorrowWrite;
  expect(
      !verify(changed).valid(),
      "conditional value flow does not grant exclusive-loan join permission");
}

void testInferredArrayReturnSuppliers() {
  const auto result = analyzeText(R"gti(
import std.string;
std::string_view pick(std::string_view first[2], std::string_view second[2]) {
  return first[0];
}
int main() {
  std::string_view first[2] = {"first", "other"};
  std::string_view second[2] = {"second", "other"};
  return pick(first, second) == "first" ? 0 : 1;
}
)gti",
                                  true);
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "array element return infers its supplying parameter");
  if (!result.canGenerateCode())
    return;
  const auto *pick = namedFunction(result, "pick");
  expect(pick && pick->returnBorrowOrigin == lang::BorrowOriginKind::Argument &&
             pick->returnBorrowParameter == 0,
         "array return identifies the first supplier rather than its "
         "same-typed neighbor");
  if (!pick)
    return;
  auto changed = result.mir;
  auto *forged = const_cast<lang::MirFunctionInstance *>(
      changed.findFunctionInstance(pick->id));
  forged->returnBorrowParameter = 1;
  forged->returnBorrowParameters = {1};
  expect(functionError(lang::verifyMirProgram(changed), pick->id,
                       "inline text array returns a dependency outside its "
                       "declared suppliers"),
         "an inferred array return cannot substitute a same-typed unrelated "
         "supplier");
}

void testArrayPublicationDiagnostic() {
  const auto result = analyzeText(R"gti(
import std.string;
mut std::string_view published = "";
void publish(std::string_view values[2]) { published = values[0]; }
int main() {
  std::string owner{"dynamic"};
  std::string_view values[2] = {owner.view(), "literal"};
  publish(values);
  return 0;
}
)gti",
                                  true);
  expect(!result.canGenerateCode(),
         "dependent array contents cannot reach static storage");
  expect(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto &d) { return d.code == "GTI-B0001"; }),
         "array publication is a source lifetime rejection, not an internal "
         "error");
  expect(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [](const auto &d) {
                       return d.code == "GTI-S2017" && d.primary.line == 8 &&
                              d.message.find("requires an ownerless value") !=
                                  std::string::npos;
                     }),
         "ownerless publication diagnostic identifies the actual call");
}

void testArrayOwnerlessPublicationContract() {
  const auto result = analyzeText(R"gti(
import std.string;
mut std::string_view published = "";
void publish(std::string_view values[2]) { published = values[0]; }
int main() {
  std::string_view values[2] = {"first", "second"};
  publish(values);
  return published == "first" ? 0 : 1;
}
)gti",
                                  true);
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "literal array contents can reach static storage");
  if (!result.canGenerateCode())
    return;
  const auto *publisher = namedFunction(result, "publish");
  expect(publisher &&
             publisher->ownerlessTextParameters == std::vector<std::size_t>{0},
         "publishing an array element requires ownerless contents from its "
         "formal");
  if (!publisher)
    return;
  auto changed = result.mir;
  const_cast<lang::MirFunctionInstance *>(
      changed.findFunctionInstance(publisher->id))
      ->ownerlessTextParameters.clear();
  expect(
      functionError(lang::verifyMirProgram(changed), publisher->id,
                    "ownerless text input contract differs"),
      "an array publisher cannot erase its independently derived requirement");
}

void testBorrowedArrayProjectionProof() {
  const auto result = analyzeText(R"gti(
import std.string;
int main() {
  std::string first{"first"}; std::string second{"second"};
  std::string_view views[2] = {first.view(), second.view()};
  return views[0] == "first" && views[1] == "second" ? 0 : 1;
}
)gti",
                                  true);
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "borrowed array literals retain all element owners");
  if (!result.canGenerateCode())
    return;
  const auto *main = namedFunction(result, "main");
  expect(main != nullptr, "borrowed array proof has a main body");
  if (!main)
    return;
  auto changed = main->body;
  for (auto &loan : changed.loans)
    loan.carriers.clear();
  std::size_t checked = 0;
  for (const auto &place : changed.places) {
    if (place.type.kind != lang::SemanticType::StringView ||
        place.root != lang::MirPlaceRootKind::Binding ||
        place.projections.empty() ||
        !std::all_of(place.projections.begin(), place.projections.end(),
                     [](const auto &part) {
                       return part.kind == lang::MirProjectionKind::Index;
                     }))
      continue;
    ++checked;
    expect(!lang::mir_detail::isOwnerlessTextStorageProjection(result.mir,
                                                               changed, place),
           "actual array stores prevent forged ownerless reads without carrier "
           "annotations");
  }
  expect(checked >= 2,
         "both indexed borrowed elements have independent projection checks");
}

void testGenericArrayReceiverRetention() {
  const auto result = analyzeText(R"gti(
struct Entries<T, uint64_t Capacity> {
  mut T values[Capacity];
  mut uint64_t used;
  Entries():values({}),used(0){}
  bool insert(T value) mut {
    if (this.used == Capacity) { return false; }
    this.values[this.used] = value;
    this.used += 1;
    return true;
  }
  bool forward(T value) mut { return this.insert(value); }
};
int main() {
  mut Entries<std::string_view, 3> values{};
  if (!values.insert("first") || !values.forward("second")) { return 1; }
  mut Entries<int, 3> integers{};
  return integers.insert(1) && integers.forward(2) ? 0 : 2;
}
)gti");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &function : result.mir.functionInstances()) {
      const auto *info = result.semantics.findFunction(function.declaration);
      if (!info || info->qualifiedName.find("Entries::") != 0)
        continue;
      std::cerr << "generic method " << function.id << ' '
                << info->qualifiedName << " declared ownerless inputs:";
      for (const auto index : function.ownerlessTextParameters)
        std::cerr << ' ' << index;
      std::cerr << "\n";
    }
  }
  expect(result.canGenerateCode(),
         "generic inline storage accepts its ownerless control");
  if (!result.canGenerateCode())
    return;
  for (const auto name : {"Entries::insert", "Entries::forward"}) {
    const auto *function = namedFunction(result, name);
    expect(function != nullptr,
           "the instantiated generic storage method reaches MIR");
    if (!function)
      continue;
    const auto effects =
        lang::deriveMirRetainingStoreEffects(*function, &result.mir);
    expect(effects.complete && effects.effects.size() == 1 &&
               effects.effects.front().destination ==
                   lang::MirCarrierRoot{lang::MirPlaceRootKind::This, 0} &&
               effects.effects.front().source ==
                   lang::MirCarrierRoot{lang::MirPlaceRootKind::Binding,
                                        function->parameterBindings.front()},
           "conditional indexed writes and forwarding retain their argument in "
           "the receiver");
  }
}

void testGenericFieldReceiverRetention() {
  const auto result = analyzeText(R"gti(
struct Cell<T> {
  mut T value;
  Cell(T initial):value(initial){}
  void put(T input) mut { this.value = input; }
  bool forward(T input) mut { this.put(input); return true; }
};
int main() {
  mut Cell<std::string_view> text{"initial"};
  if (!text.forward("stored")) { return 1; }
  mut Cell<int> number{1};
  return number.forward(2) && number.value == 2 ? 0 : 2;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "generic field forwarding preserves conditional contracts without "
         "constraining integers");
  if (!result.canGenerateCode())
    return;
  bool textSeen = false;
  bool integerSeen = false;
  for (const auto &function : result.mir.functionInstances()) {
    const auto *info = result.semantics.findFunction(function.declaration);
    if (!info || info->qualifiedName != "Cell::forward" ||
        function.parameterTypes.size() != 1)
      continue;
    const auto effects =
        lang::deriveMirRetainingStoreEffects(function, &result.mir);
    if (function.parameterTypes.front().kind ==
        lang::SemanticType::StringView) {
      textSeen = true;
      expect(effects.complete && effects.effects.size() == 1,
             "generic text field forwarding retains its input");
    } else if (function.parameterTypes.front().kind ==
               lang::SemanticType::Int32) {
      integerSeen = true;
      expect(effects.complete && effects.effects.empty() &&
                 function.ownerlessTextParameters.empty(),
             "the concrete integer field has no text or retention obligation");
    }
  }
  expect(textSeen && integerSeen,
         "both generic field specializations were checked");
}

void testGenericArrayElementReturnContract() {
  const auto result =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Hir})
          .analyze("generic-array-element-return.gti", R"gti(
import std.string;
struct Entries<T, uint64_t Capacity> {
  mut T values[Capacity];
  Entries():values({}){}
  T at(uint64_t index) { return this.values[index]; }
};
int main() {
  Entries<std::string_view, 3> text{};
  Entries<int, 3> numbers{};
  return text.at(0) == "" && numbers.at(0) == 0 ? 0 : 1;
}
)gti",
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!result.semanticValid || !result.hirValid)
    printDiagnostics(result);
  expect(result.semanticValid && result.hirValid,
         "the generic array getter reaches checked HIR");
  bool text = false;
  bool scalar = false;
  for (const auto &function : result.hir.functionInstances()) {
    if (!function.source || function.source->name().lexeme != "at")
      continue;
    if (function.returnType.kind == lang::SemanticType::StringView) {
      text = true;
      expect(function.returnBorrowOrigin == lang::BorrowOriginKind::Receiver &&
                 function.returnBorrowFields.size() == 1 &&
                 function.returnBorrowFields.front().resultFields.empty() &&
                 function.returnBorrowFields.front().sourceFields.size() == 1,
             "an indexed text getter exports the stored field's external "
             "dependency");
    } else if (function.returnType.kind == lang::SemanticType::Int32) {
      scalar = true;
      expect(function.returnBorrowOrigin == lang::BorrowOriginKind::None &&
                 function.returnBorrowFields.empty(),
             "an ordinary scalar element does not acquire a text lifetime "
             "contract");
    }
  }
  expect(text && scalar,
         "both concrete generic getter specializations were inspected");
}

void testGenericArrayElementReturnProof() {
  const auto result = analyzeText(R"gti(
import std.string;
struct Entries<T, uint64_t Capacity> {
  mut T values[Capacity];
  Entries():values({}){}
  T at(uint64_t index) { return this.values[index]; }
};
int main() {
  Entries<std::string_view, 3> text{};
  Entries<int, 3> numbers{};
  return text.at(0) == "" && numbers.at(0) == 0 ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "the generic text-array getter has an independently verified return "
         "proof");
  if (!result.canGenerateCode())
    return;
  bool inspected = false;
  for (const auto &function : result.mir.functionInstances()) {
    if (function.returnType.kind != lang::SemanticType::StringView ||
        function.returnBorrowFields.empty())
      continue;
    // Find the actual array-field relation rather than depending on a generic
    // specialization's generated name or ordinal.
    bool arrayField = false;
    for (const auto &relation : function.returnBorrowFields)
      for (const auto field : relation.sourceFields)
        for (const auto &record : result.mir.classInstances())
          for (const auto &member : record.declaredFields)
            if (member.symbol == field &&
                member.type.kind == lang::SemanticType::Array)
              arrayField = true;
    if (!arrayField)
      continue;
    inspected = true;
    auto changed = result.mir;
    auto *getter = const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(function.id));
    getter->returnBorrowOrigin = lang::BorrowOriginKind::None;
    getter->returnBorrowFields.clear();
    expect(!lang::verifyMirProgram(changed).valid(),
           "an indexed getter cannot erase its external stored-text return "
           "relation");
  }
  expect(inspected,
         "the array getter's return relation was independently challenged");
}

void testRetainingStoreContractSeparation() {
  const auto result = analyzeText(R"gti(
import std.string;
mut std::string_view persistent = "";
struct Holder {
  mut std::string_view value;
  Holder():value(""){}
  void retain(std::string_view input) mut { this.value = input; }
  void retain_and_escape(std::string_view input) mut {
    this.value = input;
    persistent = input;
  }
};
int main() {
  mut Holder holder{};
  holder.retain("first");
  holder.retain_and_escape("second");
  return 0;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "the store-contract controls compile with literal inputs");
  if (!result.canGenerateCode())
    return;
  for (const auto &[name, permitted] :
       {std::pair{"Holder::retain", true},
        std::pair{"Holder::retain_and_escape", false}}) {
    const auto *function = namedFunction(result, name);
    expect(function != nullptr, "the store-contract method reaches MIR");
    if (!function)
      continue;
    auto changed = result.mir;
    auto *target = const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(function->id));
    target->ownerlessTextParameters.clear();
    expect(lang::verifyMirProgram(changed).valid() == permitted,
           permitted ? "an independently modeled retaining write can discharge "
                       "its own ownerless requirement"
                     : "a retaining write cannot discharge a separate "
                       "persistent escape of the same input");
  }
}

void testCapturedRetainingStoreEscape() {
  const std::string prefix = R"gti(
import std.string;
struct Holder {
  mut std::string_view value;
  Holder():value(""){}
  void put(std::string_view input) mut { this.value = input; }
};
int main() {
  mut Holder holder{};
)gti";
  const auto control = analyzeText(prefix + R"gti(
  auto literal = []() -> std::string_view { return "literal"; };
  holder.put(literal());
  return holder.value == "literal" ? 0 : 1;
}
)gti");
  if (!control.canGenerateCode())
    printDiagnostics(control);
  expect(control.canGenerateCode(),
         "a literal lambda result can enter retaining storage");
  const auto escape = analyzeText(prefix + R"gti(
  {
    std::string owner{"short"};
    auto view = owner.view();
    auto captured = [view]() -> std::string_view { return view; };
    holder.put(captured());
  }
  return holder.value == "short" ? 0 : 1;
}
)gti");
  const bool safe = rejectedAtSource(escape, "GTI-S2017") ||
                    rejectedAtSource(escape, "GTI-S2045");
  if (!safe)
    printDiagnostics(escape);
  expect(safe, "retaining a captured view cannot outlive its underlying owner");
}

void testRetainingCallableEscape() {
  // An erased invocation must not lose a receiver-retention obligation.
  // Ownerless writes and invocation-local stores need no escaping contract;
  // a literal actual cannot justify erasing a target's retaining contract.
  const std::string prefix = R"gti(
import std.string;
import std.functional;
struct Holder {
  mut std::string_view value;
  Holder():value(""){}
  void put(std::string_view input) mut { this.value = input; }
};
int main() {
  std::function_ptr<void(mut Holder&, std::string_view)> store{
    [](mut Holder& target, std::string_view input) -> void {
      target.put(input);
    }
  };
  mut Holder holder{};
)gti";
  auto localOnly = prefix;
  const auto write = localOnly.find("target.put(input);");
  expect(write != std::string::npos,
         "the callable control has its explicit store");
  if (write != std::string::npos)
    localOnly.replace(write, std::string_view("target.put(input);").size(),
                      "target.put(\"literal\");");
  const auto control = analyzeText(localOnly + R"gti(
  store(holder, "literal");
  return holder.value == "literal" ? 0 : 1;
}
)gti");
  if (!control.canGenerateCode())
    printDiagnostics(control);
  expect(control.canGenerateCode(), "an invocation pointer storing ownerless "
                                    "text has no retaining-input contract");
  auto localStorage = prefix;
  localStorage.replace(localStorage.find("target.put(input);"),
                       std::string_view("target.put(input);").size(),
                       "mut Holder local{}; local.put(input);");
  const auto local = analyzeText(localStorage + R"gti(
  std::string owner{"local"};
  store(holder, owner.view());
  return 0;
}
)gti");
  if (!local.canGenerateCode())
    printDiagnostics(local);
  expect(local.canGenerateCode(),
         "a lambda can retain an input in storage local to its invocation");
  const auto erased = analyzeText(prefix + R"gti(
  store(holder, "literal");
  return 0;
}
)gti");
  expect(rejectedAtSource(erased, "GTI-S2045"),
         "even a literal invocation cannot erase a retaining-input contract");
  const auto escape = analyzeText(prefix + R"gti(
  { std::string owner{"short"}; store(holder, owner.view()); }
  return holder.value == "short" ? 0 : 1;
}
)gti");
  const bool safe = rejectedAtSource(escape, "GTI-S2017") ||
                    rejectedAtSource(escape, "GTI-S2045");
  if (!safe) {
    std::cerr << "retaining callable escape code generation: "
              << escape.canGenerateCode() << '\n';
    printDiagnostics(escape);
  }
  expect(safe, "an erased callable cannot leave a view of a destroyed owner in "
               "its receiver");
}

void testOpaqueRetainingDirectEscape() {
  const std::string declarations = R"gti(
import std.string;
extern "C" { void opaque_effect(); }
struct Holder {
  mut std::string_view value;
  Holder():value(""){}
  void put(std::string_view input) mut { this.value = input; }
};
void retain_opaque(mut Holder& target, std::string_view input) {
  target.put(input); opaque_effect();
}
void copy_opaque(mut std::string& target, std::string_view input) {
  target.append(input); opaque_effect();
}
)gti";
  const auto owned = analyzeText(declarations + R"gti(
int main() {
  mut std::string output{""};
  { std::string owner{"short"}; copy_opaque(output, owner.view()); }
  return output.view() == "short" ? 0 : 1;
}
)gti");
  if (!owned.canGenerateCode())
    printDiagnostics(owned);
  expect(owned.canGenerateCode(),
         "copying text into owned storage does not retain its source lifetime");
  const auto escape = analyzeText(declarations + R"gti(
int main() {
  mut Holder holder{};
  { std::string owner{"short"}; retain_opaque(holder, owner.view()); }
  return holder.value == "short" ? 0 : 1;
}
)gti");
  const bool safe = rejectedAtSource(escape, "GTI-S2045") ||
                    rejectedAtSource(escape, "GTI-S2017");
  if (!safe) {
    std::cerr << "opaque direct retaining call code generation: "
              << escape.canGenerateCode() << '\n';
    printDiagnostics(escape);
  }
  expect(safe, "an opaque direct call cannot hide its retained input lifetime");
}

void testOpaqueRetainingLambdaEscape() {
  const auto escape = analyzeText(R"gti(
import std.string;
import std.functional;
extern "C" { void opaque_effect(); }
struct Holder {
  mut std::string_view value;
  Holder():value(""){}
  void put(std::string_view input) mut { this.value = input; }
};
void retain_opaque(mut Holder& target, std::string_view input) {
  target.put(input);
  opaque_effect();
}
int main() {
  std::function_ptr<void(mut Holder&, std::string_view)> store{
    [](mut Holder& target, std::string_view input) -> void {
      retain_opaque(target, input);
    }
  };
  mut Holder holder{};
  { std::string owner{"short"}; store(holder, owner.view()); }
  return holder.value == "short" ? 0 : 1;
}
)gti");
  const auto *opaque = namedFunction(escape, "retain_opaque");
  expect(opaque != nullptr, "the lambda's opaque retaining helper reaches MIR");
  if (!rejectedAtSource(escape, "GTI-S2045")) {
    std::cerr << "opaque retaining lambda code generation: "
              << escape.canGenerateCode() << '\n';
    printDiagnostics(escape);
  }
  expect(rejectedAtSource(escape, "GTI-S2045"),
         "a lambda cannot hide a retaining obligation behind an opaque helper");
}

void testRetainingNamedCallableEscape() {
  const std::string declarations = R"gti(
import std.string;
import std.functional;
struct Holder {
  mut std::string_view value;
  Holder():value(""){}
  void put(std::string_view input) mut { this.value = input; }
};
void retain(mut Holder& target, std::string_view input) { target.put(input); }
void literal(mut Holder& target, std::string_view input) { target.put("literal"); }
)gti";
  const auto control = analyzeText(declarations + R"gti(
int main() {
  std::function_ptr<void(mut Holder&, std::string_view)> store{literal};
  mut Holder holder{};
  std::string owner{"short"}; store(holder, owner.view());
  return holder.value == "literal" ? 0 : 1;
}
)gti");
  if (!control.canGenerateCode())
    printDiagnostics(control);
  expect(control.canGenerateCode(), "a named callable storing only literal "
                                    "text has no input-retention obligation");
  const auto direct = analyzeText(declarations + R"gti(
int main() {
  std::string owner{"direct"};
  mut Holder holder{}; retain(holder, owner.view());
  return holder.value == "direct" ? 0 : 1;
}
)gti");
  if (!direct.canGenerateCode())
    printDiagnostics(direct);
  expect(direct.canGenerateCode(),
         "direct calls preserve a named function's retaining-input relation");
  const auto escape = analyzeText(declarations + R"gti(
int main() {
  std::function_ptr<void(mut Holder&, std::string_view)> store{retain};
  mut Holder holder{};
  { std::string owner{"short"}; store(holder, owner.view()); }
  return holder.value == "short" ? 0 : 1;
}
)gti");
  const bool safe = rejectedAtSource(escape, "GTI-S2045");
  if (!safe) {
    std::cerr << "named retaining callable escape code generation: "
              << escape.canGenerateCode() << '\n';
    printDiagnostics(escape);
  }
  expect(safe,
         "a named function pointer cannot erase an input-retention obligation");
  const auto uncertain = analyzeText(declarations + R"gti(
extern "C" { void opaque_effect(); }
void retain_opaque(mut Holder& target, std::string_view input) {
  target.put(input);
  opaque_effect();
}
int main() {
  std::function_ptr<void(mut Holder&, std::string_view)> store{retain_opaque};
  mut Holder holder{};
  { std::string owner{"short"}; store(holder, owner.view()); }
  return holder.value == "short" ? 0 : 1;
}
)gti");
  const auto *opaque = namedFunction(uncertain, "retain_opaque");
  expect(opaque &&
             !lang::deriveMirRetainingStoreEffects(*opaque, &uncertain.mir)
                  .complete,
         "the opaque-call control reaches MIR with an incomplete retention "
         "summary");
  if (!rejectedAtSource(uncertain, "GTI-S2045"))
    printDiagnostics(uncertain);
  expect(rejectedAtSource(uncertain, "GTI-S2045"),
         "an incomplete effect summary cannot certify retention-free callable "
         "conversion");
}

void testRetainingCallConditionalCleanupProof() {
  const auto result = analyzeText(R"gti(
import std.string;
struct Sink {
  mut std::string_view text;
  Sink():text(""){}
  void set(std::string_view value) mut { this.text = value; }
};
int main() {
  std::string owner{"text"};
  std::string_view view = owner.view();
  mut Sink sink{};
  sink.set("literal");
  return int(view.size());
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "conditional call cleanup has a valid source control");
  if (!result.canGenerateCode())
    return;
  const auto *main = namedFunction(result, "main");
  const auto *setter = namedFunction(result, "Sink::set");
  expect(main && setter, "the cleanup proof has both source bodies");
  if (!main || !setter)
    return;
  auto body = main->body;
  lang::MirLoanId dependency = 0;
  lang::MirOperand borrowed;
  lang::MirInstructionId next = 1;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions) {
      next = std::max(next, instruction.id + 1);
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.loan && instruction.result &&
          instruction.info.type.kind == lang::SemanticType::StringView) {
        dependency = *instruction.loan;
        borrowed = {.kind = lang::MirOperandKind::Value,
                    .value = *instruction.result,
                    .type = instruction.info.type};
      }
    }
  expect(dependency != 0, "the control produces an external dependency");
  if (!dependency)
    return;
  // Exercise loan-flow verification separately from the still-closed source
  // storage contract. Replace the literal setter input with the real view SSA.
  bool changedInput = false;
  for (auto &block : body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.functionTarget == setter->id &&
          instruction.operands.size() == 1) {
        auto *input = &instruction.operands.front();
        if (input->kind == lang::MirOperandKind::Value)
          for (auto &inputBlock : body.blocks)
            for (auto &stage : inputBlock.instructions)
              if (stage.result == input->value &&
                  stage.kind == lang::MirInstructionKind::CallInput &&
                  stage.operands.size() == 1)
                input = &stage.operands.front();
        *input = borrowed;
        changedInput = true;
      }
  expect(changedInput, "the setter receives the represented dependency");
  if (!changedInput)
    return;
  const auto proof = [&](const lang::MirBody &candidate,
                         const lang::MirProgram *program) {
    return lang::verifyMirPersistentStorageLoanFlow(
        candidate, main->id, {}, nullptr, nullptr, {}, program);
  };
  expect(proof(body, &result.mir).valid(),
         "the call's actual retained input is live");
  body.findBlock(body.entry)
      ->instructions.insert(body.findBlock(body.entry)->instructions.begin(),
                            {.id = next,
                             .kind = lang::MirInstructionKind::EndBorrow,
                             .loan = dependency});
  expect(!proof(body, nullptr).valid(),
         "body-only verification cannot invent a retaining call contract");
  const auto checked = proof(body, &result.mir);
  if (!checked.valid())
    for (const auto &error : checked.errors)
      std::cerr << error.message << '\n';
  expect(checked.valid(),
         "proven call retention permits cleanup before a conditional producer");
  // Retained shared groups may end between lexical drops and their boundary
  // marker. Their endpoints have no runtime action; loan flow must validate
  // them without losing the boundary's exact runtime drop sequence.
  auto cleanup = body;
  bool insertedEndpoint = false;
  for (auto &block : cleanup.blocks) {
    for (auto position = block.instructions.begin();
         position != block.instructions.end(); ++position)
      if (position->cleanupBoundaryEnd &&
          !cleanup.cleanupBoundaries[position->cleanupBoundaryEnd - 1]
               .obligations.empty()) {
        block.instructions.insert(position,
                                  {.id = next + 1,
                                   .kind = lang::MirInstructionKind::EndBorrow,
                                   .loan = dependency});
        insertedEndpoint = true;
        break;
      }
    if (insertedEndpoint)
      break;
  }
  expect(insertedEndpoint,
         "the control has a cleanup boundary with runtime drops");
  expect(lang::rebuildMirValueUses(cleanup),
         "the changed call input's SSA uses are rebuilt");
  const auto cleanupProof = lang::verifyMirBody(cleanup, main->id, &result.mir);
  if (!cleanupProof.valid())
    for (const auto &error : cleanupProof.errors)
      std::cerr << error.message << '\n';
  expect(cleanupProof.valid(),
         "checked shared endpoints preserve the exact cleanup drop sequence");
  auto nonRetaining = result.mir;
  auto *changedSetter = const_cast<lang::MirFunctionInstance *>(
      nonRetaining.findFunctionInstance(setter->id));
  for (auto &block : changedSetter->body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Assign)
        instruction.tombstone = true;
  expect(!proof(body, &nonRetaining).valid(),
         "removing the callee store removes conditional cleanup authority");
}

void testRetainingStoreEffectsOnLoweredProgram() {
  const auto result = analyzeText(R"gti(
import std.string;
struct TextSlot {
  mut std::string_view text;
  TextSlot():text(""){}
  void set(std::string_view value) mut { this.text = value; }
  void forward(std::string_view value) mut { this.set(value); }
  void repeat(std::string_view first, std::string_view second, int depth) mut {
    if (depth > 0) { this.repeat(second, first, depth - 1); }
    this.set(first);
  }
};
int main() {
  mut TextSlot slot{};
  slot.repeat("first", "second", 2);
  slot.forward("earlier");
  slot.set("literal");
  return slot.text == "literal" ? 0 : 1;
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "the existing ownerless setter control compiles");
  if (!result.canGenerateCode())
    return;
  const auto *setter = namedFunction(result, "TextSlot::set");
  expect(setter != nullptr, "the setter has a source MIR body");
  if (!setter)
    return;
  const auto effects = lang::deriveMirRetainingStoreEffects(*setter);
  expect(effects.complete && effects.effects.size() == 1,
         "the setter's retaining effect is reconstructed from its real MIR");
  if (effects.effects.size() == 1)
    expect(effects.effects[0].destination ==
                   lang::MirCarrierRoot{lang::MirPlaceRootKind::This, 0} &&
               effects.effects[0].source ==
                   lang::MirCarrierRoot{lang::MirPlaceRootKind::Binding,
                                        setter->parameterBindings.front()},
           "the setter stores its input into its receiver");
  const auto *forwarder = namedFunction(result, "TextSlot::forward");
  expect(forwarder != nullptr, "the retaining helper has a source MIR body");
  if (forwarder) {
    const auto composed =
        lang::deriveMirRetainingStoreEffects(*forwarder, &result.mir);
    expect(
        composed.complete && composed.effects.size() == 1 &&
            composed.effects[0].destination ==
                lang::MirCarrierRoot{lang::MirPlaceRootKind::This, 0} &&
            composed.effects[0].source ==
                lang::MirCarrierRoot{lang::MirPlaceRootKind::Binding,
                                     forwarder->parameterBindings.front()},
        "a source helper preserves the nested setter's retaining obligation");
  }
  const auto *recursive = namedFunction(result, "TextSlot::repeat");
  expect(recursive != nullptr, "the recursive retaining helper is lowered");
  if (recursive) {
    const auto summary =
        lang::deriveMirRetainingStoreEffects(*recursive, &result.mir);
    expect(summary.complete && summary.effects.size() == 2,
           "recursive source helper converges to both permuted inputs");
    for (std::size_t index = 0; index < 2; ++index)
      expect(std::find(summary.effects.begin(), summary.effects.end(),
                       lang::MirRetainingStoreEffect{
                           {lang::MirPlaceRootKind::This, 0},
                           {lang::MirPlaceRootKind::Binding,
                            recursive->parameterBindings[index]}}) !=
                 summary.effects.end(),
             "recursive source helper preserves each receiver/input relation");
  }
  const auto *main = namedFunction(result, "main");
  expect(main != nullptr, "the setter has an ordinary source caller");
  if (!main)
    return;
  const auto callerFlow = lang::deriveMirCarrierFlow(main->body, &result.mir);
  bool applied = false;
  for (const auto &block : main->body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.functionTarget == setter->id) {
        const auto targets = callerFlow.storeTargets.find(instruction.id);
        const auto inputs = callerFlow.storeInputs.find(instruction.id);
        applied =
            targets != callerFlow.storeTargets.end() &&
            targets->second.size() == 1 &&
            targets->second.front().kind == lang::MirPlaceRootKind::Binding &&
            inputs != callerFlow.storeInputs.end() && inputs->second &&
            inputs->second->empty();
      }
  expect(applied,
         "the callee effect is substituted into the caller's actual receiver");
  // Exercise the same verified callee with represented dynamic inputs and
  // independently check source provenance before the receiver union.
  lang::MirBody caller;
  caller.entry = 1;
  for (std::size_t id = 1; id <= 4; ++id)
    caller.places.push_back(
        {.id = id, .root = lang::MirPlaceRootKind::Binding, .binding = id});
  caller.loans = {{.id = 1, .kind = lang::MirLoanKind::Stored, .source = 2},
                  {.id = 2, .kind = lang::MirLoanKind::Stored, .source = 3}};
  const lang::MirOperand receiver{.kind = lang::MirOperandKind::Copy,
                                  .place = 1};
  caller.blocks = {
      {.id = 1,
       .instructions =
           {{.id = 1,
             .kind = lang::MirInstructionKind::Initialize,
             .destination = 1,
             .operands = {{.kind = lang::MirOperandKind::Constant}}},
            {.id = 2,
             .kind = lang::MirInstructionKind::Call,
             .receiver = receiver,
             .operands = {{.kind = lang::MirOperandKind::Loan, .loan = 1}},
             .functionTarget = setter->id},
            {.id = 3,
             .kind = lang::MirInstructionKind::Initialize,
             .destination = 4,
             .operands = {receiver}},
            {.id = 4,
             .kind = lang::MirInstructionKind::Call,
             .receiver = receiver,
             .operands = {{.kind = lang::MirOperandKind::Loan, .loan = 2}},
             .functionTarget = setter->id},
            {.id = 5,
             .kind = lang::MirInstructionKind::Load,
             .operands = {receiver}},
            {.id = 6,
             .kind = lang::MirInstructionKind::Load,
             .operands = {{.kind = lang::MirOperandKind::Copy, .place = 4}}}},
       .terminator = {.kind = lang::MirTerminatorKind::Return}}};
  const auto retained = lang::deriveMirCarrierFlow(caller, &result.mir);
  const lang::MirCarrierRoot formalInput{lang::MirPlaceRootKind::Binding,
                                         setter->parameterBindings.front()};
  expect(retained.retainingInputs.at(2).at(formalInput) ==
                 lang::MirCarriedLoans{std::vector<lang::MirLoanId>{1}} &&
             retained.retainingInputs.at(4).at(formalInput) ==
                 lang::MirCarriedLoans{std::vector<lang::MirLoanId>{2}},
         "retaining calls preserve each actual source proof before receiver "
         "union");
  expect(retained.complete &&
             retained.reads.at(5) ==
                 lang::MirCarriedLoans{std::vector<lang::MirLoanId>{1, 2}} &&
             retained.reads.at(6) ==
                 lang::MirCarriedLoans{std::vector<lang::MirLoanId>{1}},
         "successive retaining calls widen the receiver while copies keep "
         "independent owners");
  caller.blocks[0].instructions.insert(
      caller.blocks[0].instructions.begin() + 1,
      {.id = 7,
       .kind = lang::MirInstructionKind::CallInput,
       .callInputKind = lang::HirCallInputKind::MutableBorrow,
       .result = 1,
       .operands = {receiver}});
  for (auto &instruction : caller.blocks[0].instructions)
    if (instruction.kind == lang::MirInstructionKind::Call)
      instruction.receiver =
          lang::MirOperand{.kind = lang::MirOperandKind::Value, .value = 1};
  const auto staged = lang::deriveMirCarrierFlow(caller, &result.mir);
  expect(staged.complete && staged.reads.at(5) == retained.reads.at(5) &&
             staged.reads.at(6) == retained.reads.at(6) &&
             staged.reads.at(4) ==
                 lang::MirCarriedLoans{std::vector<lang::MirLoanId>{1, 2}},
         "staging a borrowed receiver preserves its physical destination");
  caller.blocks[0].instructions[1].callInputKind =
      lang::HirCallInputKind::CopyValue;
  const auto copiedReceiver = lang::deriveMirCarrierFlow(caller, &result.mir);
  expect(!copiedReceiver.complete && !copiedReceiver.storeTargets.contains(2),
         "an independent receiver copy must not alias the original storage");
}

void testOwnedStorageInitialization() {
  const auto result = analyzeText(R"gti(
import std.memory;
int main() {
  mut std::uninitialized<int> storage{};
  unsafe {
    int* address = storage.data();
    std::initialize_owned_at(address, storage, 42);
    return *address == 42 ? 0 : 1;
  }
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "owner-aware scalar initialization reaches verified MIR");
}

void testOwnedStorageReferenceArgument() {
  const auto result = analyzeText(R"gti(
import std.memory;
import std.string;
int inspect<T>(T& storage) { return 0; }
int main() {
  std::string owner{"borrowed"};
  mut std::uninitialized<std::string_view> storage{};
  unsafe { std::initialize_owned_at(storage.data(), storage, owner.view()); }
  return inspect(storage);
}
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(), "a retained dependency does not replace a "
                                   "storage owner's reference identity");
  const auto *main = namedFunction(result, "main");
  expect(main != nullptr, "owned storage reference probe has a main body");
  bool checkedShell = false;
  if (main)
    for (const auto &block : main->body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.callInputKind == lang::HirCallInputKind::ReadBorrow &&
            instruction.callInputRole == lang::MirCallInputRole::Argument &&
            instruction.callInputIndex == 0 &&
            instruction.operands.size() == 1 &&
            instruction.operands.front().kind ==
                lang::MirOperandKind::BorrowRead)
          checkedShell = true;
  expect(checkedShell, "reference input borrows the physical owner instead of "
                       "its stored external dependency");
}

void testOwnedStorageBorrowedRecord() {
  const auto result = analyzeText(R"gti(
import std.memory;
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main() {
  int owner = 42;
  mut std::uninitialized<Reading> storage{};
  unsafe {
    Reading* address = storage.data();
    std::initialize_owned_at(address, storage, Reading(owner));
    auto selected = std::borrow_owned_at(address, storage);
    return selected.read() == 42 ? 0 : 1;
  }
}
)gti");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &finding :
         lang::auditMirPersistentStorageBorrows(result.mir))
      std::cerr << "AUDIT " << finding.body.owner << ':' << finding.block << ':'
                << finding.instruction << ' ' << finding.detail << '\n';
    for (const auto name :
         {"main", "std::borrow_owned_at", "std::initialize_owned_at"})
      if (const auto *function = namedFunction(result, name)) {
        for (const auto &error :
             lang::verifyMirBody(function->body, function->id).errors)
          std::cerr << "BODY " << name << ':' << error.instruction << ' '
                    << error.message << '\n';
        std::cerr << "TRACE " << name << '\n'
                  << lang::MirPrinter().print(function->body);
      }
  }
  if (const auto *initializer =
          namedFunction(result, "std::initialize_owned_at")) {
    const auto summary =
        lang::deriveMirRetainingStoreEffects(*initializer, &result.mir);
    expect(summary.complete && !summary.effects.empty(),
           "generated record cleanup preserves the initialization retention "
           "summary");
    // An unmodeled replacement cleanup must not inherit the pure-drop fact.
    auto changed = result.mir;
    auto *function = const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(initializer->id));
    bool mutated = false;
    for (auto &block : function->body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Drop) {
          instruction.kind = lang::MirInstructionKind::CleanupBeforeAssign;
          mutated = true;
        }
    expect(
        mutated &&
            !lang::deriveMirRetainingStoreEffects(*function, &changed).complete,
        "unknown cleanup does not acquire a non-retaining summary");
  }
  expect(result.canGenerateCode(), "owned-storage reads preserve shell and "
                                   "referent dependencies separately");
  if (!result.canGenerateCode())
    return;
}

void testOwnedStorageReadWrapper() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.memory;
unsafe T&<a> read_slot<T, Owner, lifetime a>(const T* address, Owner&<a> owner) {
  unsafe { return std::borrow_owned_at(address, owner); }
}
int main() {
  std::string owner{"borrowed"};
  mut std::uninitialized<std::string_view> storage{};
  unsafe {
    std::string_view* address = storage.data();
    std::initialize_owned_at(address, storage, owner.view());
    std::string_view selected = read_slot(address, storage);
    return selected == "borrowed" ? 0 : 1;
  }
}
)gti");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &finding :
         lang::auditMirPersistentStorageBorrows(result.mir))
      std::cerr << "AUDIT " << finding.body.owner << ':' << finding.block << ':'
                << finding.instruction << ' ' << finding.detail << '\n';
    for (const auto name : {"main", "read_slot", "std::borrow_owned_at",
                            "std::initialize_owned_at"})
      if (const auto *function = namedFunction(result, name)) {
        for (const auto &error :
             lang::verifyMirBody(function->body, function->id).errors)
          std::cerr << "BODY " << name << ':' << error.instruction << ' '
                    << error.message << '\n';
        std::cerr << "TRACE " << name << '\n'
                  << lang::MirPrinter().print(function->body);
      }
  }
  expect(result.canGenerateCode(), "owned-storage reads preserve shell and "
                                   "referent dependencies separately");
  if (!result.canGenerateCode())
    return;
  const auto *reader = namedFunction(result, "std::borrow_owned_at");
  expect(reader != nullptr, "wrapper contents proof reaches its source reader");
  if (!reader)
    return;
  const auto *wrapper = namedFunction(result, "read_slot");
  expect(wrapper != nullptr, "contents-forwarding wrapper is represented");
  if (!wrapper)
    return;
  auto cycle = result.mir;
  auto *cyclicWrapper = const_cast<lang::MirFunctionInstance *>(
      cycle.findFunctionInstance(wrapper->id));
  for (auto &block : cyclicWrapper->body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.functionTarget == reader->id)
        instruction.functionTarget = wrapper->id;
  bool checkedContents = false;
  for (const auto &loan : wrapper->body.loans)
    if (lang::mir_detail::ownedContentEntry(wrapper->body, loan, &result.mir)) {
      checkedContents = true;
      const auto *cyclicLoan = cyclicWrapper->body.findLoan(loan.id);
      expect(cyclicLoan && !lang::mir_detail::ownedContentEntry(
                               cyclicWrapper->body, *cyclicLoan, &cycle),
             "a helper cycle cannot invent an owner-contents primitive");
    }
  expect(checkedContents,
         "helper contents entry reaches its concrete primitive");
  auto changed = result.mir;
  auto *callee = const_cast<lang::MirFunctionInstance *>(
      changed.findFunctionInstance(reader->id));
  for (auto &block : callee->body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.intrinsic == lang::IntrinsicKind::ObjectBorrowOwned)
        instruction.intrinsic = lang::IntrinsicKind::RawBorrow;
  expect(!lang::verifyMirProgram(changed).valid(),
         "helper signatures cannot replace the concrete owner-contents proof");
}

void testOwnedStorageMultipleOwners() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.memory;
class RawSlots<std::layout_queryable T> {
public:
  mut std::uninitialized<T> left{};
  mut std::uninitialized<T> right{};
  RawSlots() = default;
};
int main() {
  std::string first{"first"}; std::string second{"second"};
  mut RawSlots<std::string_view> storage{};
  unsafe {
    std::string_view* left = storage.left.data();
    std::string_view* right = storage.right.data();
    std::initialize_owned_at(left, storage, first.view());
    std::initialize_owned_at(right, storage, second.view());
    std::string_view selected = std::borrow_owned_at(right, storage);
    return selected == "second" ? 0 : 1;
  }
}
)gti");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &finding :
         lang::auditMirPersistentStorageBorrows(result.mir))
      std::cerr << "AUDIT " << finding.body.owner << ':' << finding.block << ':'
                << finding.instruction << ' ' << finding.detail << '\n';
    for (const auto name :
         {"main", "std::borrow_owned_at", "std::initialize_owned_at"})
      if (const auto *function = namedFunction(result, name)) {
        for (const auto &error :
             lang::verifyMirBody(function->body, function->id).errors)
          std::cerr << "BODY " << name << ':' << error.instruction << ' '
                    << error.message << '\n';
        std::cerr << "TRACE " << name << '\n'
                  << lang::MirPrinter().print(function->body);
      }
  }
  expect(result.canGenerateCode(), "owned-storage reads preserve shell and "
                                   "referent dependencies separately");
  if (!result.canGenerateCode())
    return;
}

void testOwnedStorageMultipleRecords() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.memory;
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
class RawSlots<std::layout_queryable T> {
public:
  mut std::uninitialized<T> left{};
  mut std::uninitialized<T> right{};
  RawSlots() = default;
};
int main() {
  int first = 11; int second = 22;
  mut RawSlots<Reading> storage{};
  unsafe {
    Reading* left = storage.left.data();
    Reading* right = storage.right.data();
    std::initialize_owned_at(left, storage, Reading(first));
    std::initialize_owned_at(right, storage, Reading(second));
    Reading selected = std::borrow_owned_at(right, storage);
    return selected.read() == 22 ? 0 : 1;
  }
}
)gti");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &finding :
         lang::auditMirPersistentStorageBorrows(result.mir))
      std::cerr << "AUDIT " << finding.body.owner << ':' << finding.block << ':'
                << finding.instruction << ' ' << finding.detail << '\n';
    for (const auto name :
         {"main", "std::borrow_owned_at", "std::initialize_owned_at"})
      if (const auto *function = namedFunction(result, name)) {
        for (const auto &error :
             lang::verifyMirBody(function->body, function->id).errors)
          std::cerr << "BODY " << name << ':' << error.instruction << ' '
                    << error.message << '\n';
        std::cerr << "TRACE " << name << '\n'
                  << lang::MirPrinter().print(function->body);
      }
  }
  expect(result.canGenerateCode(), "owned-storage reads preserve shell and "
                                   "referent dependencies separately");
  if (!result.canGenerateCode())
    return;
  auto forged = result.mir;
  bool changed = false;
  const auto *reader = namedFunction(result, "std::borrow_owned_at");
  expect(reader != nullptr, "multiple-record accessor is represented");
  if (!reader)
    return;
  auto *callee = const_cast<lang::MirFunctionInstance *>(
      forged.findFunctionInstance(reader->id));
  for (auto &block : callee->body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.intrinsic == lang::IntrinsicKind::ObjectBorrowOwned) {
        instruction.intrinsic = lang::IntrinsicKind::RawBorrow;
        changed = true;
      }
  expect(changed, "multiple-record read uses the explicit contents accessor");
  expect(!lang::verifyMirProgram(forged).valid(),
         "multiple record owners do not authorize an unproved raw accessor");
}

void testOwnedStorageReferenceReceiver() {
  for (const int projected : {0, 1, 2}) {
    std::string source = R"gti(
import std.string;
import std.vector;
std::string clone_first(std::vector<std::string>& values) {
  return values[std::size_t(0)].clone();
}
int main() {
  mut std::vector<std::string> values{};
  values.push_back(std::string("kept"));
  return clone_first(values).size() == 4 ? 0 : 1;
}
)gti";
    if (projected) {
      const auto insert = source.find("std::string clone_first");
      source.insert(insert, "struct State { std::vector<std::string> values; "
                            "State(std::vector<std::string> values) : "
                            "values(std::move(values)) {} };\n");
      const auto signature =
          source.find("clone_first(std::vector<std::string>& values)");
      source.replace(
          signature,
          std::string("clone_first(std::vector<std::string>& values)").size(),
          "clone_first(State& state)");
      source.replace(source.find("return values["),
                     std::string("return values[").size(),
                     "return state.values[");
      source.replace(
          source.find("return clone_first(values)"),
          std::string("return clone_first(values)").size(),
          "State state{std::move(values)}; return clone_first(state)");
    }
    if (projected == 2)
      source.replace(source.find("state.values[std::size_t(0)]"),
                     std::string("state.values[std::size_t(0)]").size(),
                     "state.values.back()");
    const auto result = analyzeText(source, true);
    if (!result.canGenerateCode())
      printDiagnostics(result);
    expect(
        result.canGenerateCode(),
        "owned contents accessed through a reference formal's receiver lower");
    if (!result.canGenerateCode())
      return;
    const auto *function = namedFunction(result, "clone_first");
    expect(function != nullptr, "reference receiver helper is represented");
    if (!function)
      return;
    bool checked = false;
    for (const auto &loan : function->body.loans) {
      if (!lang::mir_detail::ownedContentEntry(function->body, loan,
                                               &result.mir))
        continue;
      auto forged = function->body;
      bool changed = false;
      for (auto &place : forged.places)
        if (!place.projections.empty() &&
            place.projections.front().kind ==
                lang::MirProjectionKind::Dereference) {
          place.projections.front().kind = lang::MirProjectionKind::Index;
          changed = true;
        }
      expect(changed, "reference receiver has an exact dereference place");
      expect(
          !lang::mir_detail::ownedContentEntry(forged, loan, &result.mir),
          "an arbitrary projection cannot witness the reference formal owner");
      if (projected == 2) {
        auto wrongStage = function->body;
        bool changedStage = false;
        for (auto &block : wrongStage.blocks)
          for (auto &instruction : block.instructions)
            if (instruction.kind == lang::MirInstructionKind::CallInput &&
                instruction.callInputRole == lang::MirCallInputRole::Receiver) {
              instruction.callSite = 0;
              changedStage = true;
            }
        expect(changedStage,
               "method receiver has an explicit call-input stage");
        expect(
            !lang::mir_detail::ownedContentEntry(wrongStage, loan, &result.mir),
            "a stage from another call cannot witness the physical owner");
      }
      checked = true;
    }
    expect(checked,
           "reference receiver has a verified external-contents entry");
  }
}

void testOwnedStorageReceiverWrite() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.utility;
import std.memory;
class RawBox<std::layout_queryable T> {
public:
  mut std::uninitialized<T> slot{};
  RawBox() = default;
  unsafe void write(T value) mut requires std::move_constructible<T> { unsafe { std::initialize_owned_at(this.slot.data(), this, std::move(value)); } }
  unsafe T& read() {
    unsafe { return std::borrow_owned_at(this.slot.data(), this); }
  }
};
int main() {
  std::string owner{"borrowed"};
  mut RawBox<std::string_view> box{};
  unsafe {
    box.write(owner.view());
    std::string_view selected = box.read();
    return selected == "borrowed" ? 0 : 1;
  }
}

)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(), "generic storage insertion compiles");
  for (const auto name :
       {"RawBox::write", "std::initialize_owned_at", "main"}) {
    const auto *function = namedFunction(result, name);
    expect(function != nullptr, "storage insertion has a source body");
    if (!function)
      continue;
    const auto effects =
        lang::deriveMirRetainingStoreEffects(*function, &result.mir);
    if (std::string_view(name) == "RawBox::write")
      expect(effects.complete && effects.effects.size() == 1 &&
                 effects.effects.front().destination.kind ==
                     lang::MirPlaceRootKind::This &&
                 effects.effects.front().source ==
                     lang::MirCarrierRoot{lang::MirPlaceRootKind::Binding,
                                          function->parameterBindings.front()},
             "generic storage insertion retains its input in the receiver");
    if (std::string_view(name) == "RawBox::write") {
      auto unknown = *function;
      bool changed = false;
      for (auto &block : unknown.body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.kind == lang::MirInstructionKind::Call &&
              instruction.unsafeOperation != lang::UnsafeOperationKind::None &&
              instruction.functionTarget) {
            instruction.functionTarget.reset();
            changed = true;
          }
      expect(changed, "insertion delegates to an unsafe source helper");
      expect(
          !lang::deriveMirRetainingStoreEffects(unknown, &result.mir).complete,
          "an unsafe call without a checked target keeps unknown effects");
    }
    if (!effects.complete && std::string_view(name) != "main")
      std::cerr << "TRACE " << name << '\n'
                << lang::MirPrinter().print(function->body);
  }
}

void testOwnedStorageReceiverRead() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.memory;
class RawBox<std::layout_queryable T> {
public:
  mut std::uninitialized<T> slot{};
  RawBox() = default;
  unsafe T& read() {
    unsafe { return std::borrow_owned_at(this.slot.data(), this); }
  }
};
int main() {
  std::string owner{"borrowed"};
  mut RawBox<std::string_view> box{};
  unsafe {
    std::initialize_owned_at(box.slot.data(), box, owner.view());
    std::string_view selected = box.read();
    return selected == "borrowed" ? 0 : 1;
  }
}
)gti");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &finding :
         lang::auditMirPersistentStorageBorrows(result.mir))
      std::cerr << "AUDIT " << finding.body.owner << ':' << finding.block << ':'
                << finding.instruction << ' ' << finding.detail << '\n';
    for (const auto name : {"main", "RawBox::read", "std::borrow_owned_at",
                            "std::initialize_owned_at"})
      if (const auto *function = namedFunction(result, name)) {
        for (const auto &error :
             lang::verifyMirBody(function->body, function->id).errors)
          std::cerr << "BODY " << name << ':' << error.instruction << ' '
                    << error.message << '\n';
        std::cerr << "TRACE " << name << '\n'
                  << lang::MirPrinter().print(function->body);
      }
  }
  expect(result.canGenerateCode(), "owned-storage reads preserve shell and "
                                   "referent dependencies separately");
  if (!result.canGenerateCode())
    return;
  const auto *reader = namedFunction(result, "RawBox::read");
  expect(reader != nullptr, "receiver reader has a checked source body");
  if (!reader)
    return;
  bool checked = false;
  for (const auto &loan : reader->body.loans)
    if (lang::mir_detail::ownedContentEntry(reader->body, loan, &result.mir)) {
      checked = true;
      auto forged = loan;
      forged.access = lang::AccessMode::Mutable;
      expect(!lang::mir_detail::ownedContentEntry(reader->body, forged,
                                                  &result.mir),
             "shared receiver contents do not authorize mutable dependencies");
    }
  expect(checked,
         "receiver contents are independently traced to the primitive");
}

void testOwnedStorageBorrowedRead() {
  const auto result = analyzeText(R"gti(
import std.memory;
import std.string;
int main() {
  std::string owner{"borrowed"};
  mut std::uninitialized<std::string_view> storage{};
  unsafe {
    std::string_view* address = storage.data();
    std::initialize_owned_at(address, storage, owner.view());
    std::string_view selected = std::borrow_owned_at(address, storage);
    return selected == "borrowed" ? 0 : 1;
  }
}
)gti");
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &finding :
         lang::auditMirPersistentStorageBorrows(result.mir))
      std::cerr << "AUDIT " << finding.body.owner << ':' << finding.block << ':'
                << finding.instruction << ' ' << finding.detail << '\n';
    for (const auto name :
         {"main", "std::borrow_owned_at", "std::initialize_owned_at"})
      if (const auto *function = namedFunction(result, name)) {
        for (const auto &error :
             lang::verifyMirBody(function->body, function->id).errors)
          std::cerr << "BODY " << name << ':' << error.instruction << ' '
                    << error.message << '\n';
        std::cerr << "TRACE " << name << '\n'
                  << lang::MirPrinter().print(function->body);
      }
  }
  expect(result.canGenerateCode(), "owned-storage reads preserve shell and "
                                   "referent dependencies separately");
  if (!result.canGenerateCode())
    return;
  const auto *reader = namedFunction(result, "std::borrow_owned_at");
  expect(reader != nullptr, "owner-aware reader has a checked source body");
  if (!reader)
    return;
  bool foundRead = false;
  for (const auto &block : reader->body.blocks)
    for (const auto &read : block.instructions) {
      if (read.intrinsic != lang::IntrinsicKind::ObjectBorrowOwned ||
          !read.loan)
        continue;
      foundRead = true;
      const auto *resultLoan = reader->body.findLoan(*read.loan);
      expect(resultLoan && resultLoan->storedDependency != 0,
             "owner-aware read carries a separate contents entry");
      if (!resultLoan || !resultLoan->storedDependency)
        continue;
      for (int mutation = 0; mutation < 3; ++mutation) {
        auto changed = result.mir;
        auto &body = const_cast<lang::MirFunctionInstance *>(
                         changed.findFunctionInstance(reader->id))
                         ->body;
        if (mutation == 0) {
          // An ordinary raw address assertion does not establish contents.
          for (auto &candidateBlock : body.blocks)
            for (auto &candidate : candidateBlock.instructions)
              if (candidate.id == read.id)
                candidate.intrinsic = lang::IntrinsicKind::RawBorrow;
        } else if (mutation == 1) {
          for (auto &loan : body.loans)
            if (loan.id == resultLoan->id)
              loan.storedDependency = 0;
        } else {
          for (auto &loan : body.loans)
            if (loan.id == resultLoan->storedDependency)
              loan.access = lang::AccessMode::Mutable;
        }
        expect(!lang::verifyMirProgram(changed).valid(),
               "missing or forged owner-content authority is rejected");
      }
    }
  expect(foundRead, "owner-aware reader lowers to the explicit primitive");
}

void testProcessAllocatorRetention() {
  const auto frontend = lang::Frontend().analyze("allocator-retention.gti",
                                                 "int main() { return 0; }");
  expect(frontend.canGenerateCode(), "allocator retention fixture compiles");
  using Operation = lang::ProcessMemoryRuntimeOperation;
  for (unsigned value = static_cast<unsigned>(Operation::NativeSizeMax);
       value < static_cast<unsigned>(Operation::Count); ++value) {
    const auto operation = static_cast<Operation>(value);
    const auto *signature = lang::processMemoryRuntimeSignature(operation);
    if (!signature)
      continue;
    lang::MirFunctionInstance function;
    function.id = 1;
    function.definitionKind = lang::MirDefinitionKind::Declaration;
    function.linkage = lang::LanguageLinkage::C;
    function.externalSymbol = signature->symbol;
    function.returnType = signature->returnType;
    function.parameterTypes = signature->parameters;
    function.processMemoryOperation = operation;
    const bool selected = lang::processMemoryRuntimeSelectedForTarget(
        operation, frontend.mir.target().os);
    const auto summary =
        lang::deriveMirRetainingStoreEffects(function, &frontend.mir);
    expect(
        summary.complete == selected && summary.effects.empty(),
        "only target-selected exact allocator operations prove non-retention");
    auto changed = function;
    changed.externalSymbol += "_untrusted";
    expect(
        !lang::deriveMirRetainingStoreEffects(changed, &frontend.mir).complete,
        "an arbitrary native symbol cannot borrow the allocator contract");
    changed = function;
    changed.processMemoryOperation = Operation::None;
    expect(
        !lang::deriveMirRetainingStoreEffects(changed, &frontend.mir).complete,
        "an unclassified native declaration remains unknown");
    changed = function;
    changed.parameterTypes.push_back(lang::SemanticType::referenceTo(
        lang::SemanticType::StringView, lang::AccessMode::Mutable));
    expect(
        !lang::deriveMirRetainingStoreEffects(changed, &frontend.mir).complete,
        "a changed allocator signature cannot conceal checked reference "
        "retention");
  }
}

void testRetainingLocalConstruction() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.utility;
import std.memory;
class Scratch {
  int value;
public:
  Scratch(int input) : value(input) {}
};
class RawBox<std::layout_queryable T> {
public:
  mut std::uninitialized<T> slot{};
  RawBox() = default;
  unsafe void write(T value) mut requires std::move_constructible<T> { Scratch scratch{7}; unsafe { std::initialize_owned_at(this.slot.data(), this, std::move(value)); } }
  unsafe T& read() {
    unsafe { return std::borrow_owned_at(this.slot.data(), this); }
  }
};
int main() {
  std::string owner{"borrowed"};
  mut RawBox<std::string_view> box{};
  unsafe {
    box.write(owner.view());
    std::string_view selected = box.read();
    return selected == "borrowed" ? 0 : 1;
  }
}

)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "local construction preserves a retaining method contract");
  const auto *writer = namedFunction(result, "RawBox::write");
  expect(writer != nullptr,
         "retaining method with local construction is lowered");
  if (!writer)
    return;
  expect(lang::deriveMirRetainingStoreEffects(*writer, &result.mir).complete,
         "a checked constructor writing only private storage has no outward "
         "retention");
  // A constructor body that writes a formal is not private construction.
  // Mutate the already-lowered body to ensure that this proof checks actual
  // destinations, not just the constructor's nominal identity.
  auto outward = result.mir;
  bool outwardWrite = false;
  for (const auto &block : writer->body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.constructorTarget) {
        auto *constructor = const_cast<lang::MirConstructorInstance *>(
            outward.findConstructorInstance(*instruction.constructorTarget));
        if (!constructor || constructor->parameterBindings.empty())
          continue;
        lang::MirPlaceId formalPlace = 0;
        for (const auto &place : constructor->body.places)
          if (place.root == lang::MirPlaceRootKind::Binding &&
              place.projections.empty() &&
              place.binding == constructor->parameterBindings.front())
            formalPlace = place.id;
        for (auto &candidateBlock : constructor->body.blocks)
          for (auto &candidate : candidateBlock.instructions)
            if (formalPlace &&
                candidate.kind == lang::MirInstructionKind::Initialize) {
              candidate.destination = formalPlace;
              outwardWrite = true;
            }
      }
  expect(outwardWrite &&
             !lang::deriveMirRetainingStoreEffects(*writer, &outward).complete,
         "a constructor with an outward formal write is not classified as "
         "private");
  auto unknown = *writer;
  bool changed = false;
  for (auto &block : unknown.body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Construct &&
          instruction.constructorTarget) {
        instruction.constructorTarget.reset();
        changed = true;
      }
  expect(
      changed &&
          !lang::deriveMirRetainingStoreEffects(unknown, &result.mir).complete,
      "construction without an inspectable target cannot claim no outward "
      "stores");
}

void testRetainingLocalCleanup() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.utility;
import std.memory;
class Scratch {
  mut int value;
public:
  Scratch(int input) : value(input) {}
  ~Scratch() { this.value = 0; }
};
class RawBox<std::layout_queryable T> {
public:
  mut std::uninitialized<T> slot{};
  RawBox() = default;
  unsafe void write(T value) mut requires std::move_constructible<T> { Scratch scratch{7}; unsafe { std::initialize_owned_at(this.slot.data(), this, std::move(value)); } }
  unsafe T& read() {
    unsafe { return std::borrow_owned_at(this.slot.data(), this); }
  }
};
int main() {
  std::string owner{"borrowed"};
  mut RawBox<std::string_view> box{};
  unsafe {
    box.write(owner.view());
    std::string_view selected = box.read();
    return selected == "borrowed" ? 0 : 1;
  }
}

)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "private cleanup preserves a retaining method contract");
  const auto *writer = namedFunction(result, "RawBox::write");
  expect(writer != nullptr, "retaining method with local cleanup is lowered");
  if (!writer)
    return;
  expect(lang::deriveMirRetainingStoreEffects(*writer, &result.mir).complete,
         "an inspected destructor with only private writes has no outward "
         "retention");
  auto unknown = result.mir;
  bool changed = false;
  for (const auto &destructor : result.mir.destructorInstances())
    if (destructor.definitionKind == lang::MirDefinitionKind::Source) {
      const_cast<lang::MirDestructorInstance *>(
          unknown.findDestructorInstance(destructor.id))
          ->definitionKind = lang::MirDefinitionKind::Declaration;
      changed = true;
    }
  expect(changed &&
             !lang::deriveMirRetainingStoreEffects(*writer, &unknown).complete,
         "an unavailable destructor body cannot claim private cleanup");
}

void testRetainingExplicitCleanup() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.utility;
import std.memory;
class Scratch {
  mut int value;
public:
  Scratch(int input) : value(input) {}
  ~Scratch() { this.value = 0; }
};
class RawBox<std::layout_queryable T> {
public:
  mut std::uninitialized<T> slot{};
  RawBox() = default;
  unsafe void write(T value) mut requires std::move_constructible<T> { mut std::uninitialized<Scratch> scratch{}; unsafe { std::initialize_owned_at(scratch.data(), scratch, Scratch(7)); std::destroy_at(scratch.data()); } unsafe { std::initialize_owned_at(this.slot.data(), this, std::move(value)); } }
  unsafe T& read() {
    unsafe { return std::borrow_owned_at(this.slot.data(), this); }
  }
};
int main() {
  std::string owner{"borrowed"};
  mut RawBox<std::string_view> box{};
  unsafe {
    box.write(owner.view());
    std::string_view selected = box.read();
    return selected == "borrowed" ? 0 : 1;
  }
}

)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "typed raw cleanup preserves a retaining method contract");
  const auto *writer = namedFunction(result, "RawBox::write");
  const auto *destroyer = namedFunction(result, "std::destroy_at");
  expect(writer && destroyer,
         "explicit destruction and retaining writer are lowered");
  if (!writer || !destroyer)
    return;
  const auto writerEffects =
      lang::deriveMirRetainingStoreEffects(*writer, &result.mir);
  if (!writerEffects.complete)
    for (const auto &function : result.mir.functionInstances()) {
      const auto *info = result.semantics.findFunction(function.declaration);
      if (!info || function.definitionKind != lang::MirDefinitionKind::Source)
        continue;
      const auto effects =
          lang::deriveMirRetainingStoreEffects(function, &result.mir);
      if (!effects.complete) {
        std::cerr << "CLEANUP INCOMPLETE " << info->qualifiedName
                  << " id=" << function.id << '\n';
        if (info->qualifiedName == "RawBox::write" ||
            info->qualifiedName == "std::initialize_owned_at" ||
            info->qualifiedName == "std::destroy_at")
          std::cerr << lang::MirPrinter().print(function.body);
      }
    }
  if (!writerEffects.complete)
    for (const auto &block : writer->body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.constructorTarget) {
          const auto *constructor = result.mir.findConstructorInstance(
              *instruction.constructorTarget);
          if (constructor) {
            std::cerr << "CLEANUP CONSTRUCTOR " << constructor->id
                      << " definition="
                      << static_cast<int>(constructor->definitionKind)
                      << " generated-default=" << constructor->generatedDefault
                      << '\n';
            std::cerr << lang::MirPrinter().print(constructor->body);
          }
        }
  expect(
      writerEffects.complete,
      "explicit cleanup derives its retention from the exact destroyed type");
  auto scheduled = result.mir;
  bool changedDefault = false;
  std::vector<const lang::MirBody *> constructionBodies{&writer->body};
  std::vector<lang::HirConstructorInstanceId> visitedConstructors;
  for (std::size_t index = 0; index < constructionBodies.size(); ++index)
    for (const auto &block : constructionBodies[index]->blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.constructorTarget &&
            std::find(visitedConstructors.begin(), visitedConstructors.end(),
                      *instruction.constructorTarget) ==
                visitedConstructors.end()) {
          visitedConstructors.push_back(*instruction.constructorTarget);
          if (const auto *constructor = result.mir.findConstructorInstance(
                  *instruction.constructorTarget))
            constructionBodies.push_back(&constructor->body);
        }
        if (lang::mir_detail::generatedDefaultConstruction(instruction))
          for (const auto &owner : result.mir.classInstances())
            if (owner.type == instruction.info.type) {
              const_cast<lang::MirClassInstance *>(
                  scheduled.findClassInstance(owner.id))
                  ->generatedDefaultRequiresSchedule = true;
              changedDefault = true;
            }
      }
  expect(
      changedDefault &&
          !lang::deriveMirRetainingStoreEffects(*writer, &scheduled).complete,
      "generated construction cannot ignore a required initializer schedule");
  for (unsigned mutation = 0; mutation != 2; ++mutation) {
    auto changed = *destroyer;
    bool found = false;
    for (auto &block : changed.body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.intrinsic == lang::IntrinsicKind::ObjectDestroy) {
          expect(instruction.elementCleanup.has_value(),
                 "source destructor has an exact cleanup recipe");
          if (mutation == 0)
            instruction.elementCleanup.reset();
          else
            instruction.unsafeOperation = lang::UnsafeOperationKind::None;
          found = true;
        }
    expect(found && !lang::deriveMirRetainingStoreEffects(changed, &result.mir)
                         .complete,
           "missing cleanup or invalid lifetime-operation shape cannot prove "
           "non-retention");
  }
}

void testSymbolicAggregateRetention() {
  const auto result = analyzeText(R"gti(
class Payload {
  int value;
public:
  Payload(int input) : value(input) {}
};
class Box {
  mut Payload value{0};
public:
  Box() = default;
  void set(Payload input) mut { this.value = input; }
};
int main() { mut Box box{}; box.set(Payload(7)); return 0; }
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(), "ordinary aggregate assignment compiles");
  const auto *setter = namedFunction(result, "Box::set");
  expect(setter != nullptr, "aggregate setter is lowered");
  if (!setter)
    return;
  const auto original = setter->body;
  const auto effects =
      lang::deriveMirRetainingStoreEffects(*setter, &result.mir);
  expect(effects.complete && effects.effects.size() == 1 &&
             effects.effects.front().destination.kind ==
                 lang::MirPlaceRootKind::This &&
             effects.effects.front().source ==
                 lang::MirCarrierRoot{lang::MirPlaceRootKind::Binding,
                                      setter->parameterBindings.front()},
         "aggregate contents are modeled symbolically even without nominal "
         "borrowed fields");
  expect(setter->body == original,
         "summary-only tokens do not mutate executable MIR");
}

void testOpaqueOwnerUnknownRetention() {
  const auto typed = analyzeText(R"gti(
import std.string;
import std.utility;
import std.memory;
extern "C" { void opaque_effect(); }
class RawBox<std::layout_queryable T> {
public:
  mut std::uninitialized<T> slot{};
  RawBox() = default;
  unsafe void write(T value) mut requires std::move_constructible<T> { unsafe { opaque_effect(); std::initialize_owned_at(this.slot.data(), this, std::move(value)); } }
  unsafe T& read() {
    unsafe { return std::borrow_owned_at(this.slot.data(), this); }
  }
};
int main() {
  std::string owner{"borrowed"};
  mut RawBox<std::string_view> box{};
  unsafe {
    box.write(owner.view());
    std::string_view selected = box.read();
    return selected == "borrowed" ? 0 : 1;
  }
}

)gti");
  expect(rejectedAtSource(typed, "GTI-S2045"),
         "unknown effects cannot conceal borrowed text retained in typed raw "
         "slots");
  const auto erased = analyzeText(R"gti(
import std.string;
import std.memory;
extern "C" { void opaque_effect(); }
class Owner {
public:
  Owner() = default;
  unsafe void write(std::string_view* address, std::string_view value) mut {
    unsafe { opaque_effect(); std::initialize_owned_at(address, this, value); }
  }
};
int main() {
  std::string text{"borrowed"};
  mut std::uninitialized<std::string_view> slot{};
  mut Owner owner{};
  unsafe { owner.write(slot.data(), text.view()); }
  return 0;
}
)gti");
  expect(rejectedAtSource(erased, "GTI-S2045"),
         "explicit owner publication remains visible even without nominal "
         "storage fields");
}

void testNestedLifecycleRetention() {
  const auto result = analyzeText(R"gti(
import std.utility;
void construct_helper() {}
void cleanup_helper() {}
class Scratch {
public:
  Scratch() { construct_helper(); }
  ~Scratch() { cleanup_helper(); }
};
class Payload {
public:
  int value;
  Payload(int input) : value(input) {}
  ~Payload() { cleanup_helper(); }
};
void assign(mut Payload& target, Payload input) {
  Scratch scratch{};
  target = std::move(input);
}
int main() { mut Payload target{0}; assign(target, Payload(7)); return target.value; }
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "retaining store with nested lifecycle helpers compiles");
  const auto *writer = namedFunction(result, "assign");
  expect(writer != nullptr, "nested lifecycle retaining writer is lowered");
  if (!writer)
    return;
  const auto valid = lang::deriveMirRetainingStoreEffects(*writer, &result.mir);
  expect(valid.complete && valid.effects.size() == 1,
         "lifecycle helpers preserve the actual retaining transfer");
  for (unsigned mutation = 0; mutation != 2; ++mutation) {
    auto changed = *writer;
    bool found = false;
    for (auto &block : changed.body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::CleanupBeforeAssign) {
          if (mutation == 0)
            instruction.replacementCleanup.reset();
          else
            instruction.replacementPair = 0;
          found = true;
        }
    expect(found && !lang::deriveMirRetainingStoreEffects(changed, &result.mir)
                         .complete,
           "replacement cleanup needs its exact recipe and paired commit");
  }
  for (const auto name : {"construct_helper", "cleanup_helper"}) {
    const auto *helper = namedFunction(result, name);
    expect(helper != nullptr, "nested lifecycle helper has an instance");
    if (!helper)
      continue;
    auto changed = result.mir;
    const_cast<lang::MirFunctionInstance *>(
        changed.findFunctionInstance(helper->id))
        ->definitionKind = lang::MirDefinitionKind::Declaration;
    expect(
        !lang::deriveMirRetainingStoreEffects(*writer, &changed).complete,
        "unknown nested lifecycle helper invalidates the enclosing transfer");
  }
}

void testReturnedAggregateRetention() {
  const auto result = analyzeText(R"gti(
class Payload { public: int value; Payload(int input) : value(input) {} };
Payload relay(Payload input) { return input; }
Payload choose(Payload first, Payload second, bool select) {
  if (select) { return relay(first); }
  return relay(second);
}
Payload fresh() { return Payload(3); }
class Box {
  mut Payload value{0};
public:
  Box() = default;
  void set(Payload first, Payload second, bool select) mut {
    this.value = choose(first, second, select);
  }
};
int main() { mut Box box{}; box.set(Payload(1), Payload(2), true); return fresh().value; }
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(), "aggregate result forwarding compiles");
  const auto *relay = namedFunction(result, "relay");
  const auto *choose = namedFunction(result, "choose");
  const auto *fresh = namedFunction(result, "fresh");
  const auto *setter = namedFunction(result, "Box::set");
  expect(relay && choose && fresh && setter,
         "aggregate return helpers are lowered");
  if (!relay || !choose || !fresh || !setter)
    return;
  const auto selected =
      lang::deriveMirRetainingStoreEffects(*choose, &result.mir);
  const auto independent =
      lang::deriveMirRetainingStoreEffects(*fresh, &result.mir);
  const auto stored =
      lang::deriveMirRetainingStoreEffects(*setter, &result.mir);
  expect(selected.complete && selected.returnedOrigins &&
             selected.returnedOrigins->size() == 2,
         "conditional helper results preserve both input origins");
  expect(independent.complete && independent.returnedOrigins &&
             independent.returnedOrigins->empty(),
         "fresh aggregate result has a proved empty dependency set");
  expect(stored.complete && stored.effects.size() == 2,
         "retaining stores compose through aggregate-returning helpers");
  auto changed = result.mir;
  auto *changedRelay = const_cast<lang::MirFunctionInstance *>(
      changed.findFunctionInstance(relay->id));
  bool mutated = false;
  for (auto &block : changedRelay->body.blocks)
    if (block.terminator.kind == lang::MirTerminatorKind::Return &&
        block.terminator.value) {
      block.terminator.value->kind = lang::MirOperandKind::Value;
      block.terminator.value->value = 999999;
      mutated = true;
    }
  expect(mutated &&
             !lang::deriveMirRetainingStoreEffects(*changedRelay, &changed)
                  .returnedOrigins,
         "an unproved return value is unknown, never an empty origin set");
  expect(!lang::deriveMirRetainingStoreEffects(*setter, &changed).complete,
         "unknown return provenance blocks the downstream retaining store");
}

void testReturnedTextRetention() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.utility;
T relay<T>(T input) { return std::move(input); }
class Holder {
  mut std::string_view text = "";
public:
  Holder() = default;
  void set(std::string_view input) mut { this.text = relay(input); }
};
int main() { mut Holder holder{}; holder.set("borrowed"); return 0; }
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "returned borrowed text can enter retaining storage");
  const auto *writer = namedFunction(result, "Holder::set");
  const auto *relay = namedFunction(result, "relay");
  expect(writer && relay,
         "borrowed-text helper and retaining method are lowered");
  if (!writer || !relay)
    return;
  const auto summary =
      lang::deriveMirRetainingStoreEffects(*writer, &result.mir);
  if (!summary.complete) {
    const auto returned =
        lang::deriveMirRetainingStoreEffects(*relay, &result.mir);
    std::cerr << "TEXT RETURN helper complete=" << returned.complete
              << " origins="
              << (returned.returnedOrigins
                      ? std::to_string(returned.returnedOrigins->size())
                      : "unknown")
              << '\n';
    std::cerr << lang::MirPrinter().print(relay->body);
    std::cerr << lang::MirPrinter().print(writer->body);
  }
  expect(summary.complete && summary.effects.size() == 1,
         "returned text preserves its input origin in the retaining summary");
  const auto concrete = lang::deriveMirCarrierFlow(writer->body, &result.mir);
  bool keptResultLoan = false;
  for (const auto &block : writer->body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.loan)
        for (const auto &[store, inputs] : concrete.storeInputs) {
          (void)store;
          keptResultLoan =
              keptResultLoan ||
              (inputs && std::find(inputs->begin(), inputs->end(),
                                   *instruction.loan) != inputs->end());
        }
  expect(keptResultLoan,
         "concrete text storage still carries the actual result loan");
  auto changed = result.mir;
  const_cast<lang::MirFunctionInstance *>(
      changed.findFunctionInstance(relay->id))
      ->definitionKind = lang::MirDefinitionKind::Declaration;
  expect(
      !lang::deriveMirRetainingStoreEffects(*writer, &changed).complete,
      "an unknown returning helper cannot justify a symbolic borrowed store");
}

void testFailureSnapshotRetention() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.panic;
void fail(std::string_view message) { std::panic(message); }
int main() { fail("snapshot"); return 0; }
)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(), "borrowed panic message compiles");
  const auto *relay = namedFunction(result, "fail");
  expect(relay &&
             lang::deriveMirRetainingStoreEffects(*relay, &result.mir).complete,
         "failure snapshot does not retain its borrowed message");
  bool found = false;
  for (const auto &function : result.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.intrinsic == lang::IntrinsicKind::Panic) {
          found = true;
          expect(lang::mir_detail::explicitFailure(result.mir, instruction),
                 "panic snapshot has an exact failure contract");
          for (unsigned mutation = 0; mutation != 3; ++mutation) {
            auto changed = function;
            for (auto &changedBlock : changed.body.blocks)
              for (auto &call : changedBlock.instructions)
                if (call.id == instruction.id) {
                  if (mutation == 0)
                    call.localFailureSites.clear();
                  else if (mutation == 1)
                    call.parameterTypes.clear();
                  else
                    call.rawMemoryAccess = true;
                  expect(
                      !lang::mir_detail::explicitFailure(result.mir, call),
                      "malformed failure snapshot cannot prove non-retention");
                }
            expect(!lang::deriveMirRetainingStoreEffects(changed, &result.mir)
                        .complete,
                   "retention analysis rejects an unproved failure snapshot");
          }
        }
  expect(found, "panic snapshot is lowered for the mutation regressions");
}

void testGrowableBorrowedRecordReturn() {
  lang::FrontendOptions options;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
  const auto result = lang::Frontend(options).analyze(
      "growable-record-return.gti", R"gti(
import std.vector;
class Reading<lifetime a> {
  int&<a> value;
public:
  Reading(int&<a> input) : value(input) {}
  int read() { return this.value; }
};
Reading<a> pick<lifetime a>(int&<a> first, int&<a> second) {
  mut std::vector<Reading<a>> values{};
  values.push_back(Reading<a>(first)); values.push_back(Reading<a>(second));
  Reading<a> selected = values[0];
  values.reserve(32);
  return selected;
}
int main() {
  int first = 11; int second = 22;
  Reading selected = pick(first, second);
  return selected.read() == 11 ? 0 : 1;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &function : result.mir.functionInstances()) {
      const auto *info = result.semantics.findFunction(function.declaration);
      if (info && info->qualifiedName == "pick")
        std::cerr << "RECORD RETURN " << info->qualifiedName
                  << " id=" << function.id << '\n'
                  << lang::MirPrinter().print(function.body);
    }
  }
  expect(result.canGenerateCode(),
         "copied borrowed record outlives container storage");
  if (result.canGenerateCode()) {
    const auto *pick = namedFunction(result, "pick");
    expect(pick != nullptr, "copied record return has a source body");
    if (pick) {
      auto changed = result.mir;
      auto *limited = const_cast<lang::MirFunctionInstance *>(
          changed.findFunctionInstance(pick->id));
      limited->returnBorrowParameters = {0};
      const auto verification = lang::verifyMirProgram(changed);
      // Complete summary reconstruction can reject the inconsistent supplier
      // claim before the copied-value endpoint check. Require a rejection in
      // pick's own return proof, not merely a mismatching caller annotation.
      const bool rejected =
          functionError(
              verification, pick->id,
              "copied record return uses an undeclared or local supplier") ||
          functionError(verification, pick->id,
                        "complete result dependency summary lacks independent "
                        "evidence for every reachable normal return");
      if (!rejected) {
        std::cerr << "RETURN SUPPLIER MUTATION valid=" << verification.valid()
                  << '\n';
        for (const auto &error : verification.errors)
          std::cerr << "RETURN SUPPLIER ERROR owner=" << error.owner << " "
                    << error.message << '\n';
      }
      expect(rejected,
             "return publication cannot discard another possible supplier");
    }
  }
}

void testCopiedReferenceParameterProof() {
  lang::FrontendOptions options;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
  const auto result = lang::Frontend(options).analyze(
      "copy-reference-proof.gti", R"gti(
import std.string;
T copy_borrowed<T>(T& value) requires std::copy_constructible<T> { return value; }
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main() {
  int number = 42;
  Reading original{number};
  Reading record = copy_borrowed(original);
  std::string owner{"borrowed"};
  std::string_view view = owner.view();
  std::string_view copied = copy_borrowed(view);
  return copied == "borrowed" && record.read() == 42 ? 0 : 1;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!result.canGenerateCode())
    printDiagnostics(result);
  std::size_t found = 0;
  for (const auto &function : result.mir.functionInstances()) {
    const auto *info = result.semantics.findFunction(function.declaration);
    if (!info || info->qualifiedName != "copy_borrowed")
      continue;
    ++found;
    expect(lang::mir_detail::copiedReferenceParameter(function) == 0,
           "whole-value copy proves contents forwarding from its formal");
    auto changed = function;
    changed.parameterTypes[0].referenceAccess = lang::AccessMode::Mutable;
    expect(!lang::mir_detail::copiedReferenceParameter(changed),
           "mutable reference must not receive shared-copy proof");
    changed = function;
    for (auto &block : changed.body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Load)
          instruction.operands.front().kind = lang::MirOperandKind::Move;
    expect(!lang::mir_detail::copiedReferenceParameter(changed),
           "moving is not independently proved to be a contents copy");
    changed = function;
    for (auto &loan : changed.body.loans)
      if (loan.entry && loan.kind == lang::MirLoanKind::Stored)
        loan.kind = lang::MirLoanKind::Parameter;
    expect(!lang::mir_detail::copiedReferenceParameter(changed),
           "physical reference authority is not stored-contents authority");
  }
  expect(found == 2, "generic copy helper has view and user-record MIR bodies");
  expect(result.canGenerateCode(),
         "ordinary shared-reference copying is valid");
}

std::string ownedConstructionSource() {
  return R"gti(
import std.memory;
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
void put(Reading* destination, mut int& owner, int& input) {
  unsafe { std::construct_owned_at<Reading, int, int&>(destination, owner, input); }
}
int main() {
  Reading* destination = nullptr;
  mut int owner = 0;
  int number = 11;
  put(destination, owner, number);
  return 0;
}
)gti";
}

void testOwnedConstructionHir() {
  const auto source = ownedConstructionSource();
  const auto analyze = [&](const std::string &text) {
    return lang::Frontend({.stopAfter = lang::FrontendPhase::Hir})
        .analyze("owned-construction-hir.gti", text, {standardLibraryPrelude()},
                 {}, {standardLibraryRoot()});
  };
  const auto result = analyze(source);
  if (!result.semanticValid || !result.hirValid)
    printDiagnostics(result);
  expect(result.semanticValid && result.hirValid,
         "owner-aware construction selects the borrowed record constructor in "
         "HIR");
  bool found = false;
  for (const auto &function : result.hir.functionInstances())
    for (const auto &value : function.body.values)
      if (value.intrinsic == lang::IntrinsicKind::ObjectConstructOwned) {
        found = true;
        expect(
            value.constructorTarget.has_value() &&
                value.parameterTypes.size() == 3 &&
                value.parameterTypes[1].kind == lang::SemanticType::Reference &&
                value.parameterTypes[1].referenceAccess ==
                    lang::AccessMode::Mutable &&
                value.parameterTypes[2].kind == lang::SemanticType::Reference,
            "storage owner remains separate from the constructor reference "
            "parameter");
      }
  expect(found, "the owner-aware operation survives HIR lowering");
  auto readonly = source;
  readonly.replace(readonly.find("mut int& owner"),
                   std::string("mut int& owner").size(), "int& owner");
  const auto rejectedOwner = analyze(readonly);
  expect(!rejectedOwner.semanticValid,
         "owner-aware construction rejects a shared destination owner");
  auto raw = source;
  const std::string call =
      "construct_owned_at<Reading, int, int&>(destination, owner, input)";
  raw.replace(raw.find(call), call.size(),
              "construct_at<Reading, int&>(destination, input)");
  const auto rejectedRaw = analyze(raw);
  expect(!rejectedRaw.semanticValid,
         "ordinary raw construction still rejects checked borrowed payloads");
}

void testOwnedMutableBorrowHir() {
  const std::string source = R"gti(
import std.memory;
class Reading { public: int& value; Reading(int& input) : value(input) {} };
unsafe mut Reading& access(Reading* address, mut int& owner) {
  unsafe { return std::borrow_owned_mut_at(address, owner); }
}
int main() {
  Reading* address = nullptr;
  mut int owner = 0;
  unsafe { mut Reading& selected = access(address, owner); }
  return 0;
}
)gti";
  const auto analyze = [&](const std::string &text) {
    return lang::Frontend({.stopAfter = lang::FrontendPhase::Hir})
        .analyze("owned-mutable-borrow-hir.gti", text,
                 {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  };
  const auto result = analyze(source);
  if (!result.semanticValid || !result.hirValid)
    printDiagnostics(result);
  expect(result.semanticValid && result.hirValid,
         "exclusive owner-aware access preserves its checked HIR contract");
  bool found = false;
  for (const auto &function : result.hir.functionInstances())
    for (const auto &value : function.body.values)
      if (value.intrinsic == lang::IntrinsicKind::ObjectBorrowOwnedMut) {
        found = true;
        expect(
            value.parameterTypes.size() == 2 &&
                value.parameterTypes[0].pointerAccess ==
                    lang::AccessMode::Mutable &&
                value.parameterTypes[1].referenceAccess ==
                    lang::AccessMode::Mutable &&
                value.borrowAccess == lang::AccessMode::Mutable,
            "mutable access requires a writable address and exclusive owner");
      }
  expect(found, "mutable owner-aware access reaches its exact primitive");
  auto readonlyOwner = source;
  readonlyOwner.replace(readonlyOwner.find("mut int& owner"),
                        std::string("mut int& owner").size(), "int& owner");
  expect(!analyze(readonlyOwner).semanticValid,
         "shared owners cannot authorize mutable owned access");
  auto readonlyAddress = source;
  readonlyAddress.replace(readonlyAddress.find("access(Reading* address"),
                          std::string("access(Reading* address").size(),
                          "access(const Reading* address");
  expect(!analyze(readonlyAddress).semanticValid,
         "read-only pointers cannot authorize mutable owned access");
}

void testOwnedConstructionMir() {
  const auto result = lang::Frontend().analyze(
      "owned-construction-mir.gti", ownedConstructionSource(),
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "owner-aware source construction reaches verified MIR");
  if (!result.canGenerateCode())
    return;
  bool found = false;
  for (const auto &function : result.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::ObjectConstructOwned)
          continue;
        found = true;
        expect(lang::mir_detail::objectConstruct(result.mir, instruction),
               "the selected owner-aware construction has an exact structural "
               "proof");
        auto sharedOwner = instruction;
        sharedOwner.parameterTypes[1].referenceAccess =
            lang::AccessMode::ReadOnly;
        sharedOwner.operands[1].type = sharedOwner.parameterTypes[1];
        expect(
            !lang::mir_detail::objectConstruct(result.mir, sharedOwner),
            "a shared owner cannot replace the exclusive construction witness");
        auto missingOwner = instruction;
        missingOwner.parameterTypes.erase(missingOwner.parameterTypes.begin() +
                                          1);
        missingOwner.operands.erase(missingOwner.operands.begin() + 1);
        expect(!lang::mir_detail::objectConstruct(result.mir, missingOwner),
               "construction cannot erase its owner parameter");
        auto missingConstructor = instruction;
        missingConstructor.constructorTarget.reset();
        expect(
            !lang::mir_detail::objectConstruct(result.mir, missingConstructor),
            "ordinary borrowed construction cannot erase its selected "
            "constructor");
        missingOwner.intrinsic = lang::IntrinsicKind::ObjectConstruct;
        expect(
            !lang::mir_detail::objectConstruct(result.mir, missingOwner),
            "raw construction cannot inherit owner-aware payload permission");
      }
  expect(found,
         "the MIR test exercises the owner-aware construction operation");
}

void checkOwnedConstructionRecordRead(const std::string &source,
                                      bool requireMutable = false) {
  lang::FrontendOptions options;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
  const auto result = lang::Frontend(options).analyze(
      "owned-record-read.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &constructor : result.mir.constructorInstances()) {
      if (std::none_of(constructor.initializers.begin(),
                       constructor.initializers.end(),
                       [](const auto &initializer) {
                         return initializer.storesReference;
                       }))
        continue;
      std::cerr << "CONSTRUCTION CTOR id=" << constructor.id << " bindings=";
      for (const auto binding : constructor.parameterBindings)
        std::cerr << binding << ',';
      std::cerr << '\n';
      for (const auto &initializer : constructor.initializers) {
        std::cerr << "CONSTRUCTION INITIALIZER field=" << initializer.field
                  << " reference=" << initializer.storesReference
                  << " arguments=";
        for (const auto argument : initializer.arguments)
          std::cerr << argument << ',';
        std::cerr << '\n';
      }
      std::cerr << lang::MirPrinter().print(constructor.body);
    }
    for (const auto &function : result.mir.functionInstances()) {
      const auto *info = result.semantics.findFunction(function.declaration);
      if (!info || (info->qualifiedName != "main" &&
                    info->qualifiedName != "std::construct_owned_at"))
        continue;
      if (info->qualifiedName == "main" &&
          (source.find("emplace_back") != std::string::npos ||
           source.find("struct Label") != std::string::npos ||
           source.find("std::destroy_at(storage.data())") != std::string::npos))
        std::cerr << lang::MirPrinter().print(function.body);
      const auto effects =
          lang::deriveMirRetainingStoreEffects(function, &result.mir);
      const auto flow = lang::deriveMirCarrierFlow(function.body, &result.mir);
      std::cerr << "CONSTRUCTION EFFECTS " << info->qualifiedName
                << " complete=" << effects.complete
                << " stores=" << effects.effects.size() << '\n';
      std::cerr << "CONSTRUCTION FLOW " << info->qualifiedName
                << " complete=" << flow.complete
                << " effects=" << flow.effectsComplete << '\n';
      for (const auto &[instruction, inputs] : flow.reads) {
        std::cerr << "CONSTRUCTION READ instruction=" << instruction
                  << " known=" << inputs.has_value() << " loans=";
        if (inputs)
          for (const auto id : *inputs)
            std::cerr << id << ',';
        std::cerr << '\n';
      }
      for (const auto &[blockId, inputs] : flow.exits) {
        const auto *block = function.body.findBlock(blockId);
        std::cerr << "CONSTRUCTION EXIT block=" << blockId
                  << " value=" << (block ? block->terminator.hirValue : 0)
                  << " statement="
                  << (block ? block->terminator.hirStatement : 0)
                  << " known=" << inputs.has_value() << " loans=";
        if (inputs)
          for (const auto id : *inputs)
            std::cerr << id << ',';
        std::cerr << '\n';
      }
      for (const auto &[instruction, contents] : flow.resultSourceContents) {
        std::cerr << "CONSTRUCTION RESULT CONTENTS instruction=" << instruction
                  << " known=" << contents.has_value() << " loans=";
        if (contents)
          for (const auto id : *contents)
            std::cerr << id << ',';
        std::cerr << '\n';
      }
      for (const auto id : flow.incompleteEffectInstructions)
        std::cerr << "CONSTRUCTION INCOMPLETE " << info->qualifiedName
                  << " instruction=" << id << '\n';
      for (const auto &effect : effects.effects)
        std::cerr << "CONSTRUCTION STORE destination="
                  << int(effect.destination.kind) << ':'
                  << effect.destination.identity
                  << " source=" << int(effect.source.kind) << ':'
                  << effect.source.identity << '\n';
      std::cerr << "CONSTRUCTION FORMALS types="
                << function.parameterTypes.size()
                << " bindings=" << function.parameterBindings.size() << '\n';
      for (const auto binding : function.parameterBindings)
        std::cerr << "CONSTRUCTION FORMAL binding=" << binding << '\n';
      for (const auto &loan : function.body.loans) {
        if (!loan.entry)
          continue;
        const auto *place = function.body.findPlace(loan.source);
        if (!place)
          continue;
        std::cerr << "CONSTRUCTION ENTRY loan=" << loan.id
                  << " binding=" << place->binding << " projections=";
        for (const auto &projection : place->projections)
          std::cerr << int(projection.kind) << ':' << projection.index << ',';
        std::cerr << '\n';
      }
      for (const auto &[instruction, inputs] : flow.retainingInputs)
        for (const auto &[origin, loans] : inputs) {
          std::cerr << "CONSTRUCTION RETAINING instruction=" << instruction
                    << " source=" << origin.identity << " pack="
                    << (origin.packElement ? std::to_string(*origin.packElement)
                                           : "none")
                    << " known=" << loans.has_value() << " loans=";
          if (loans)
            for (const auto id : *loans)
              std::cerr << id << ',';
          std::cerr << '\n';
        }
      for (const auto &[instruction, writes] : flow.storeInputsByTarget)
        for (const auto &[target, inputs] : writes) {
          std::cerr << "CONSTRUCTION INPUT instruction=" << instruction
                    << " target=" << int(target.kind) << ':' << target.identity
                    << " known=" << inputs.has_value() << " loans=";
          if (inputs)
            for (const auto id : *inputs)
              std::cerr << id << ',';
          std::cerr << '\n';
        }
    }
  }
  expect(result.canGenerateCode(),
         "constructed borrowed records preserve their external owners on read");
  if (!result.canGenerateCode())
    return;
  if (source.find("emplace_back") != std::string::npos) {
    std::size_t checkedSnapshots = 0;
    for (const auto &function : result.mir.functionInstances()) {
      const auto *info = result.semantics.findFunction(function.declaration);
      if (!info || info->qualifiedName != "main")
        continue;
      const auto flow = lang::deriveMirCarrierFlow(function.body, &result.mir);
      for (const auto &block : function.body.blocks)
        for (const auto &call : block.instructions) {
          const auto found = flow.resultSourceContents.find(call.id);
          if (!call.loan || found == flow.resultSourceContents.end() ||
              !found->second || found->second->empty())
            continue;
          ++checkedSnapshots;
          auto changed = result.mir;
          auto *altered = const_cast<lang::MirFunctionInstance *>(
              changed.findFunctionInstance(function.id));
          altered->body.loans[*call.loan - 1].storedDependency =
              found->second->size() == 1 ? 0 : found->second->front();
          expect(!lang::verifyMirProgram(changed).valid(),
                 "a returned storage reference must preserve its exact "
                 "post-call contents");
        }
    }
    expect(checkedSnapshots >= 2,
           "emplacement must exercise distinct post-call snapshots");
  }
  if (source.find("values[0] = second.view()") != std::string::npos) {
    std::size_t checkedWrites = 0;
    for (const auto &function : result.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &write : block.instructions) {
          const auto *place = write.destination
                                  ? function.body.findPlace(*write.destination)
                                  : nullptr;
          if (write.kind != lang::MirInstructionKind::Assign || !place ||
              place->root != lang::MirPlaceRootKind::Loan ||
              place->type.kind != lang::SemanticType::StringView)
            continue;
          ++checkedWrites;
          auto changed = result.mir;
          auto *altered = const_cast<lang::MirFunctionInstance *>(
              changed.findFunctionInstance(function.id));
          altered->body.loans[place->loan - 1].access =
              lang::AccessMode::ReadOnly;
          expect(!lang::verifyMirProgram(changed).valid(),
                 "retained text writes require mutable accessor authority");
          changed = result.mir;
          altered = const_cast<lang::MirFunctionInstance *>(
              changed.findFunctionInstance(function.id));
          for (auto &changedBlock : altered->body.blocks)
            for (auto &changedWrite : changedBlock.instructions)
              if (changedWrite.id == write.id)
                changedWrite.operands.front() = lang::MirOperand{};
          expect(!lang::verifyMirProgram(changed).valid(),
                 "retained text writes require independently tracked source "
                 "contents");
        }
    expect(checkedWrites == 1,
           "replacement exercises one checked accessor store");
  }
  std::size_t dynamicRecordProofs = 0;
  for (const auto &function : result.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic != lang::IntrinsicKind::ObjectBorrowOwned ||
            instruction.info.type.kind != lang::SemanticType::Class ||
            instruction.info.traits.containsBorrowedState || !instruction.loan)
          continue;
        ++dynamicRecordProofs;
        const auto *loan = function.body.findLoan(*instruction.loan);
        expect(loan && loan->storedDependency,
               "a record with dynamic contents retains an explicit external "
               "dependency");
        if (!loan || !loan->storedDependency)
          continue;
        auto changed = result.mir;
        auto *altered = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        altered->body.loans[loan->id - 1].storedDependency = 0;
        expect(!lang::verifyMirProgram(changed).valid(),
               "erasing dynamic record contents cannot produce an ownerless "
               "reference");
        changed = result.mir;
        altered = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        altered->body.loans[loan->storedDependency - 1].access =
            lang::AccessMode::Mutable;
        expect(!lang::verifyMirProgram(changed).valid(),
               "dynamic record contents cannot upgrade external owner access");
      }
  expect(source.find("struct Label") == std::string::npos ||
             dynamicRecordProofs != 0,
         "text-field record test must exercise the general content authority "
         "proof");
  std::size_t mutableProofs = 0;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic !=
                lang::IntrinsicKind::ObjectBorrowOwnedMut ||
            !instruction.loan)
          continue;
        ++mutableProofs;
        const auto *resultLoan = function.body.findLoan(*instruction.loan);
        expect(
            resultLoan && resultLoan->parent && resultLoan->storedDependency,
            "mutable owned access keeps physical and external loans distinct");
        if (!resultLoan || !resultLoan->parent || !resultLoan->storedDependency)
          continue;
        auto changed = result.mir;
        auto *altered = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        altered->body.loans[resultLoan->parent - 1].access =
            lang::AccessMode::ReadOnly;
        expect(
            !lang::verifyMirProgram(changed).valid(),
            "mutable owned access cannot derive from a shared physical loan");
        changed = result.mir;
        altered = const_cast<lang::MirFunctionInstance *>(
            changed.findFunctionInstance(function.id));
        altered->body.loans[resultLoan->storedDependency - 1].access =
            lang::AccessMode::Mutable;
        expect(!lang::verifyMirProgram(changed).valid(),
               "exclusive storage access cannot upgrade external content "
               "authority");
      }
  }
  expect(!requireMutable || mutableProofs != 0,
         "mutable proof regression must exercise an owner-aware primitive");
  for (const auto &constructor : result.mir.constructorInstances()) {
    const auto initializer = std::find_if(
        constructor.initializers.begin(), constructor.initializers.end(),
        [](const auto &item) { return item.storesReference; });
    if (initializer == constructor.initializers.end())
      continue;
    auto changed = result.mir;
    auto *altered = const_cast<lang::MirConstructorInstance *>(
        changed.findConstructorInstance(constructor.id));
    altered->initializers[initializer - constructor.initializers.begin()]
        .arguments = {0};
    expect(!lang::verifyMirProgram(changed).valid(),
           "a reference-field publication must match its initializer argument");
    for (const auto &function : changed.functionInstances()) {
      bool invokes = false;
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions)
          invokes =
              invokes || (instruction.intrinsic ==
                              lang::IntrinsicKind::ObjectConstructOwned &&
                          instruction.constructorTarget == constructor.id);
      if (invokes)
        expect(
            !lang::deriveMirRetainingStoreEffects(function, &changed).complete,
            "unmatched constructor publication cannot certify retaining "
            "effects");
    }
  }
  for (const auto &function : result.mir.functionInstances()) {
    const auto *info = result.semantics.findFunction(function.declaration);
    if (!info || info->qualifiedName != "std::construct_owned_at")
      continue;
    for (const auto &loan : function.body.loans) {
      const auto *place = function.body.findPlace(loan.source);
      if (!loan.entry || !place || place->projections.size() != 1 ||
          place->projections.front().kind !=
              lang::MirProjectionKind::PackElement ||
          (place->type.kind != lang::SemanticType::Reference &&
           place->type.kind != lang::SemanticType::StringView))
        continue;
      auto changed = result.mir;
      auto *altered = const_cast<lang::MirFunctionInstance *>(
          changed.findFunctionInstance(function.id));
      altered->body.loans[loan.id - 1].access = lang::AccessMode::Mutable;
      expect(!lang::verifyMirProgram(changed).valid(),
             "a shared pack input cannot manufacture mutable entry authority");
      altered->body.loans[loan.id - 1] = loan;
      altered->body.loans[loan.id - 1].entry = false;
      expect(
          !lang::verifyMirProgram(changed).valid(),
          "a required pack entry cannot be removed from the verified contract");
    }
  }
}

void testOwnedConstructionRecordRead() {
  checkOwnedConstructionRecordRead(R"gti(
import std.memory;
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main() {
  int owner = 42;
  mut std::uninitialized<Reading> storage{};
  unsafe {
    Reading* address = storage.data();
    std::construct_owned_at(address, storage, Reading(owner));
    auto selected = std::borrow_owned_at(address, storage);
    std::destroy_at(address);
    return selected.read() == 42 ? 0 : 1;
  }
}
)gti");
}

void testDynamicTextCleanupCapacity() {
  const auto result = analyzeText(R"gti(
import std.string;
import std.panic;
struct Observing {
  std::string_view text;
  Observing() : text("literal") {}
  ~Observing() { if (this.text != "literal") { std::panic("bad text"); } }
};
struct Passive {
  std::string_view text;
  std::string owned;
  Passive() : text("literal"), owned("owned") {}
};
struct OwnedOnly {
  int value;
  OwnedOnly() : value(1) {}
  ~OwnedOnly() {}
};
enum PassiveChoice { text(std::string_view view), owned(std::string value), none };
enum ObservingChoice { value(Observing item), none };
struct PassiveEnum {
  PassiveChoice choice;
  PassiveEnum() : choice(PassiveChoice::none) {}
};
struct ObservingEnum {
  ObservingChoice choice;
  ObservingEnum() : choice(ObservingChoice::none) {}
};
int main() {
  PassiveEnum passive_enum{};
  ObservingEnum observing_enum{};
  Observing observing{};
  Passive passive{};
  OwnedOnly owned{};
  return observing.text == passive.text && owned.value == 1 ? 0 : 1;
}
)gti",
                                  true);
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "cleanup capacity preserves literal-only record construction");
  unsigned checked = 0;
  for (const auto &owner : result.mir.classInstances()) {
    const auto *type = result.semantics.findClassType(owner.declaration);
    if (!type)
      continue;
    if (type->qualifiedName == "Observing" ||
        type->qualifiedName == "ObservingEnum") {
      ++checked;
      expect(!owner.traits.containsBorrowedState &&
                 owner.lifecycleBoundBorrowCarrier,
             "observing cleanup retains dynamic text dependencies absent "
             "static reference traits");
      auto changed = result.mir;
      const_cast<lang::MirClassInstance *>(changed.findClassInstance(owner.id))
          ->lifecycleBoundBorrowCarrier = false;
      expect(!lang::verifyMirProgram(changed).valid(),
             "MIR independently rejects omitted dynamic cleanup retention");
    } else if (type->qualifiedName == "Passive" ||
               type->qualifiedName == "OwnedOnly" ||
               type->qualifiedName == "PassiveEnum") {
      ++checked;
      expect(!owner.lifecycleBoundBorrowCarrier,
             "passive field cleanup or a record without borrowed capacity does "
             "not pin dependencies");
    }
  }
  expect(checked == 5, "cleanup-capacity regression checks direct and "
                       "enum-nested structural cases");
}

void testBorrowedArrayCleanupContainment() {
  const auto result = analyzeText(R"gti(
mut int witness = 0;
class Observed {
  int& source;
public:
  Observed(int& input) : source(input) {}
  ~Observed() { witness += this.source; }
};
int main() {
  int first = 11;
  int second = 22;
  Observed values[2] = {Observed(first), Observed(second)};
  return 0;
}
)gti",
                                  true);
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(
      result.canGenerateCode(),
      "borrowed array elements retain dependencies through observing cleanup");
  if (!result.canGenerateCode())
    return;
  bool checked = false;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.operation != lang::MirOperation::Aggregate ||
            instruction.info.type.kind != lang::SemanticType::Array ||
            instruction.operands.size() != 2 ||
            instruction.lifecycle.size() != 3)
          continue;
        checked = true;
        for (int forgery = 0; forgery < 3; ++forgery) {
          auto changed = result.mir;
          auto *target = const_cast<lang::MirFunctionInstance *>(
              changed.findFunctionInstance(function.id));
          for (auto &changedBlock : target->body.blocks)
            for (auto &candidate : changedBlock.instructions) {
              if (candidate.id != instruction.id)
                continue;
              if (forgery == 0) {
                candidate.lifecycle.erase(candidate.lifecycle.begin());
              } else if (forgery == 1) {
                candidate.operands[1] = candidate.operands[0];
              } else {
                candidate.lifecycle.back().target =
                    candidate.lifecycle.front().source;
              }
            }
          expect(!lang::verifyMirProgram(changed).valid(),
                 "array cleanup rejects missing, unrelated or wrong-result "
                 "element transfers");
        }
      }
    }
  }
  expect(checked,
         "observing array exposes an exact aggregate cleanup transfer");
}

void testOwnedTextDestructorBoundary() {
  const std::string source = R"gti(
import std.memory;
import std.string;
import std.panic;
mut std::string_view escaped = "";
struct Label {
  std::string_view text;
  Label(std::string_view input) : text(input) {}
  ~Label() { if (this.text != "borrowed") { std::panic("expired text"); } }
};
int main() {
  std::string owner{"borrowed"};
  mut std::uninitialized<Label> storage{};
  unsafe {
    Label* address = storage.data();
    std::construct_owned_at<Label, std::uninitialized<Label>, std::string_view>(address, storage, owner.view());
    std::destroy_at(address);
  }
  return 0;
}
)gti";
  expect(rejectedAtSource(analyzeText(source, true), "GTI-S2045"),
         "dynamic text construction remains restricted for source destructors");
  // Ownerless text remains usable with ordinary user-defined cleanup.
  auto literal = source;
  const auto input = literal.find("owner.view()");
  expect(input != std::string::npos, "destructor boundary has a text input");
  if (input != std::string::npos) {
    literal.replace(input, std::string("owner.view()").size(), "\"borrowed\"");
    expect(rejectedAtSource(analyzeText(literal, true), "GTI-S2045"),
           "generic construction does not invent an ownerless guarantee for a "
           "text pack");
    literal.erase(literal.find("int main()"));
    literal += "int main() { Label value{\"borrowed\"}; return 0; }\n";
    const auto ordinary = analyzeText(literal, true);
    if (!ordinary.canGenerateCode())
      printDiagnostics(ordinary);
    expect(ordinary.canGenerateCode(),
           "ordinary ownerless text construction supports a source destructor");
  }
  auto escape = source;
  const std::string read =
      "if (this.text != \"borrowed\") { std::panic(\"expired text\"); }";
  const auto observer = escape.find(read);
  expect(observer != std::string::npos,
         "destructor fixture has an observing read");
  if (observer != std::string::npos) {
    escape.replace(observer, read.size(), "escaped = this.text;");
    expect(rejectedAtSource(analyzeText(escape, true), "GTI-S2045"),
           "destruction cannot publish borrowed text into global storage");
  }
}

void testOwnedTextConstructorEscapeBoundary() {
  const std::string source = R"gti(
import std.memory;
import std.string;
mut std::string_view escaped = "";
struct Label {
  std::string_view text;
  Label(std::string_view input) : text(input) { escaped = input; }
};
int main() {
  std::string owner{"borrowed"};
  mut std::uninitialized<Label> storage{};
  unsafe {
    Label* address = storage.data();
    std::construct_owned_at<Label, std::uninitialized<Label>, std::string_view>(address, storage, owner.view());
  }
  return escaped == "borrowed" ? 0 : 1;
}
)gti";
  expect(rejectedAtSource(analyzeText(source), "GTI-S2045"),
         "owner-tracked construction must not excuse a separate global text "
         "escape");
  auto raw = source;
  const std::string ownedCall =
      "std::construct_owned_at<Label, std::uninitialized<Label>, "
      "std::string_view>(address, storage, owner.view())";
  const auto call = raw.find(ownedCall);
  expect(call != std::string::npos,
         "escape fixture has the exact owner-tracked call");
  if (call != std::string::npos) {
    raw.replace(
        call, ownedCall.size(),
        "std::construct_at<Label, std::string_view>(address, owner.view())");
    expect(
        rejectedAtSource(analyzeText(raw), "GTI-S2045"),
        "raw construction also cannot publish an escaping dynamic text input");
  }
}

void testTextFieldInitializerStage() {
  const auto result = analyzeText(R"gti(
import std.memory;
struct Label {
  std::string_view text;
  Label(std::string_view input) : text(input) {}
};
int main() {
  Label selected{"literal"};
  return selected.text == "literal" ? 0 : 1;
}

)gti");
  if (!result.canGenerateCode())
    printDiagnostics(result);
  expect(result.canGenerateCode(),
         "text field construction retains its ordinary source contract");
  if (!result.canGenerateCode())
    return;
  std::size_t stages = 0;
  for (const auto &constructor : result.mir.constructorInstances())
    for (const auto &block : constructor.body.blocks)
      for (const auto &stage : block.instructions) {
        if (!stage.constructorInitializer ||
            stage.kind != lang::MirInstructionKind::Initialize ||
            stage.info.type.kind != lang::SemanticType::StringView)
          continue;
        ++stages;
        const auto flow =
            lang::deriveMirCarrierFlow(constructor.body, &result.mir);
        const auto targets = flow.storeTargets.find(stage.id);
        expect(targets != flow.storeTargets.end() &&
                   targets->second.size() == 1 &&
                   targets->second.front().kind == lang::MirPlaceRootKind::This,
               "constructor text publication is an explicit receiver store");
        auto changed = result.mir;
        auto *altered = const_cast<lang::MirConstructorInstance *>(
            changed.findConstructorInstance(constructor.id));
        for (auto &changedBlock : altered->body.blocks)
          for (auto &changedStage : changedBlock.instructions)
            if (changedStage.id == stage.id)
              changedStage.constructorInitializer = 0;
        expect(!lang::verifyMirProgram(changed).valid(),
               "text field initialization must retain its exact constructor "
               "stage identity");
      }
  expect(stages == 1, "one text field initializer publishes through MIR");
}

void testOwnedRecordCopyReleasesStorage() {
  checkOwnedConstructionRecordRead(R"gti(
import std.memory;
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main() {
  int owner = 42;
  mut std::uninitialized<Reading> storage{};
  { unsafe {
    std::construct_owned_at<Reading, std::uninitialized<Reading>, int&>(storage.data(), storage, owner);
  } }
  unsafe {
    auto selected = std::borrow_owned_at(storage.data(), storage);
    std::destroy_at(storage.data());
    return selected.read() == 42 ? 0 : 1;
  }
}
)gti");
}

void testOwnedConstructedTextFieldRead() {
  checkOwnedConstructionRecordRead(R"gti(
import std.memory;
import std.string;
struct Label {
  std::string_view text;
  Label(std::string_view input) : text(input) {}
};
int main() {
  std::string owner{"borrowed"};
  mut std::uninitialized<Label> storage{};
  unsafe {
    Label* address = storage.data();
    std::construct_owned_at<Label, std::uninitialized<Label>, std::string_view>(address, storage, owner.view());
    auto selected = std::borrow_owned_at(address, storage);
    return selected.text == "borrowed" ? 0 : 1;
  }
}
)gti");
}

void testOwnedRecordElementReplacementRead() {
  checkOwnedConstructionRecordRead(R"gti(
import std.vector;
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main() {
  int first = 11; int second = 22;
  mut std::vector<Reading> values{};
  values.emplace_back(Reading(first));
  auto old = values[0];
  values[0] = Reading(second);
  values.reserve(32);
  auto current = values[0];
  return old.read() == 11 && current.read() == 22 ? 0 : 1;
}
)gti");
}

void testOwnedElementReplacementRead() {
  checkOwnedConstructionRecordRead(R"gti(import std.string;
import std.utility;
import std.vector;
int main() {
  std::string first{"first"}; std::string second{"second"};
  mut std::vector<std::string_view> values{};
  values.emplace_back(first.view());
  auto old = values[0];
  values[0] = second.view();
  values.reserve(32);
  auto current = values[0];
  return old == "first" && current == "second" ? 0 : 1;
}

)gti");
}

void testOwnedEmplaceRecordRead() {
  checkOwnedConstructionRecordRead(R"gti(import std.string;
import std.utility;
import std.vector;
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main() {
  int first = 11; int second = 22;
  mut std::vector<Reading> values{};
  values.emplace_back(Reading(first));
  values.emplace_back(Reading(second));
  values.reserve(32);
  return values.size() == 2 && values[0].read() == 11 && values[1].read() == 22 ? 0 : 1;
}

)gti");
}

void testOwnedConstructionMutableRecordRead() {
  checkOwnedConstructionRecordRead(R"gti(
import std.memory;
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main() {
  int owner = 42;
  mut std::uninitialized<Reading> storage{};
  unsafe {
    Reading* address = storage.data();
    std::construct_owned_at(address, storage, Reading(owner));
    auto selected = std::borrow_owned_mut_at(address, storage);
    std::destroy_at(address);
    return selected.read() == 42 ? 0 : 1;
  }
}
)gti",
                                   true);
}

void testOwnedConstructionViewRead() {
  checkOwnedConstructionRecordRead(R"gti(
import std.memory;
import std.string;
int main() {
  std::string owner{"borrowed"};
  mut std::string_view selected = "";
  {
    mut std::uninitialized<std::string_view> storage{};
    unsafe {
      std::string_view* address = storage.data();
      std::construct_owned_at(address, storage, owner.view());
      selected = std::borrow_owned_at(address, storage);
    }
  }
  return selected == "borrowed" ? 0 : 1;
}
)gti");
}

void testOwnedConstructionPairRead() {
  checkOwnedConstructionRecordRead(R"gti(import std.memory;
class PairReading {
  int& first;
  int& second;
public:
  PairReading(int& left, int& right) : first(left), second(right) {}
  int read() { return this.first + this.second; }
};
int main() {
  int first = 11;
  int second = 22;
  mut std::uninitialized<PairReading> storage{};
  unsafe {
    PairReading* address = storage.data();
    std::construct_owned_at<PairReading, std::uninitialized<PairReading>, int&, int&>(address, storage, first, second);
    auto selected = std::borrow_owned_at(address, storage);
    std::destroy_at(address);
    return selected.read() == 33 ? 0 : 1;
  }
}
)gti");
}

void testGrowableBorrowedCloneCase(bool records, bool invalidateOwner = false) {
  lang::FrontendOptions options;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
  std::string source = records ? R"gti(
import std.vector;
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main() {
  int first = 11; int second = 22;
  mut std::vector<Reading> values{};
  values.push_back(Reading(first)); values.push_back(Reading(second));
  auto copied = values.clone();
  return copied[0].read() == 11 && copied[1].read() == 22 ? 0 : 1;
}
)gti"
                               : R"gti(
import std.string;
import std.vector;
int main() {
  std::string first{"first"}; std::string second{"second"};
  mut std::vector<std::string_view> values{};
  values.push_back(first.view()); values.push_back(second.view());
  auto copied = values.clone();
  return copied[0] == "first" && copied[1] == "second" ? 0 : 1;
}
)gti";
  if (invalidateOwner) {
    const std::string declaration =
        records ? "int first =" : "std::string first{";
    source.replace(source.find(declaration), declaration.size(),
                   "mut " + declaration);
    source.insert(source.find("  return copied"),
                  records ? "  first = 99;\n" : "  first.clear();\n");
  }
  const auto result = lang::Frontend(options).analyze(
      "growable-clone.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  if (result.canGenerateCode() == invalidateOwner) {
    printDiagnostics(result);
    for (const auto &function : result.mir.functionInstances()) {
      const auto *info = result.semantics.findFunction(function.declaration);
      if (info && (info->qualifiedName == "std::vector::clone" ||
                   info->qualifiedName.ends_with("::read_unchecked") ||
                   info->qualifiedName == "std::borrow_owned_at" ||
                   info->qualifiedName == "std::clone" ||
                   (invalidateOwner && info->qualifiedName == "main"))) {
        const auto flow =
            lang::deriveMirCarrierFlow(function.body, &result.mir);
        for (const auto id : flow.incompleteEffectInstructions)
          std::cerr << "CLONE INCOMPLETE " << info->qualifiedName
                    << " instruction=" << id << '\n';
        const auto effects =
            lang::deriveMirRetainingStoreEffects(function, &result.mir);
        std::cerr << "CLONE EFFECTS " << info->qualifiedName
                  << " complete=" << effects.complete
                  << " returned-known=" << effects.returnedOrigins.has_value();
        if (effects.returnedOrigins)
          for (const auto root : *effects.returnedOrigins)
            std::cerr << " root=" << static_cast<int>(root.kind) << ':'
                      << root.identity;
        std::cerr << '\n';
        for (const auto &[call, groups] : flow.copiedCallResults) {
          std::cerr << "CLONE COPY call=" << call
                    << " known=" << groups.has_value();
          if (groups)
            for (const auto group : *groups)
              std::cerr << " loan=" << group;
          std::cerr << '\n';
        }
        for (const auto &[call, inputs] : flow.retainingInputs)
          for (const auto &[root, groups] : inputs) {
            std::cerr << "CLONE RETAIN call=" << call
                      << " formal=" << root.identity
                      << " known=" << groups.has_value();
            if (groups)
              for (const auto group : *groups)
                std::cerr << " loan=" << group;
            std::cerr << '\n';
          }
        for (const auto &[block, groups] : flow.returned) {
          std::cerr << "CLONE RETURN " << info->qualifiedName
                    << " block=" << block << " known=" << groups.has_value();
          if (groups)
            for (const auto group : *groups)
              std::cerr << " loan=" << group;
          std::cerr << '\n';
        }
        std::cerr << "CLONE " << info->qualifiedName << " id=" << function.id
                  << '\n'
                  << lang::MirPrinter().print(function.body);
      }
    }
  }
  expect(result.canGenerateCode() != invalidateOwner,
         invalidateOwner
             ? "cloned storage rejects mutation of an external owner"
             : "cloning growable storage preserves external owners");
}

void testGrowableBorrowedClone() { testGrowableBorrowedCloneCase(false); }
void testGrowableBorrowedRecordClone() { testGrowableBorrowedCloneCase(true); }
void testGrowableBorrowedCloneRetention() {
  testGrowableBorrowedCloneCase(false, true);
}

void testGrowableBorrowedRecordAccess() {
  lang::FrontendOptions options;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
  const auto result = lang::Frontend(options).analyze(
      "growable-record-access.gti", R"gti(
import std.vector;
class Reading {
  int& value;
public:
  Reading(int& input) : value(input) {}
  int read() { return this.value; }
};
int main() {
  int first = 11; int second = 22;
  mut std::vector<Reading> values{};
  values.push_back(Reading(first)); values.push_back(Reading(second));
  values.reserve(32);
  std::vector<Reading>& shared = values;
  return shared[0].read() == 11 && shared[1].read() == 22 ? 0 : 1;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &function : result.mir.functionInstances()) {
      const auto *info = result.semantics.findFunction(function.declaration);
      if (info &&
          (info->qualifiedName == "main" ||
           info->qualifiedName == "std::vector::__gti_operator_subscript" ||
           info->qualifiedName == "std::detail::prefix_owner::read_unchecked" ||
           info->qualifiedName ==
               "gti_internal::prefix_slots::read_unchecked" ||
           info->qualifiedName == "std::borrow_owned_at"))
        std::cerr << "RECORD ACCESS " << info->qualifiedName
                  << " id=" << function.id << '\n'
                  << lang::MirPrinter().print(function.body);
    }
  }
  expect(result.canGenerateCode(),
         "shared vector access preserves multiple borrowed-record owners");
}

void testGrowableBorrowedRetention() {
  // Match executable compilation while isolating the instantiated insertion
  // path. Full library checking also exercises unrelated generic operations.
  lang::FrontendOptions options;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  options.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
  const auto result = lang::Frontend(options).analyze(
      "growable-retention.gti", R"gti(
import std.string;
import std.vector;
int main() {
  std::string owner{"borrowed"};
  mut std::vector<std::string_view> values{};
  values.push_back(owner.view());
  return values.size() == 1 ? 0 : 1;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!result.canGenerateCode()) {
    printDiagnostics(result);
    for (const auto &function : result.mir.functionInstances()) {
      const auto *info = result.semantics.findFunction(function.declaration);
      if (!info || function.definitionKind != lang::MirDefinitionKind::Source)
        continue;
      const auto effects =
          lang::deriveMirRetainingStoreEffects(function, &result.mir);
      if (!effects.complete) {
        std::cerr << "INCOMPLETE " << info->qualifiedName
                  << " id=" << function.id << '\n';
        if (info->qualifiedName == "gti_internal::try_allocate_objects" ||
            info->qualifiedName == "gti_internal::raw_buffer::try_allocate") {
          const auto flow =
              lang::deriveMirCarrierFlow(function.body, &result.mir);
          std::cerr << "ALLOCATION concrete effects=" << flow.effectsComplete
                    << '\n';
          for (const auto &block : function.body.blocks)
            for (const auto &instruction : block.instructions)
              if (instruction.functionTarget) {
                const auto *callee = result.mir.findFunctionInstance(
                    *instruction.functionTarget);
                const auto *declaration =
                    callee ? result.semantics.findFunction(callee->declaration)
                           : nullptr;
                if (callee && declaration) {
                  const auto summary = lang::deriveMirRetainingStoreEffects(
                      *callee, &result.mir);
                  std::cerr
                      << "ALLOCATION call i" << instruction.id << " "
                      << declaration->qualifiedName
                      << " complete=" << summary.complete
                      << " stores=" << summary.effects.size() << " returned="
                      << (summary.returnedOrigins
                              ? std::to_string(summary.returnedOrigins->size())
                              : "unknown")
                      << '\n';
                }
              }
        }
        if (info->qualifiedName == "std::vector::push_back" ||
            info->qualifiedName == "std::vector::reserve" ||
            info->qualifiedName == "std::vector::ensure_capacity_for_one" ||
            info->qualifiedName == "gti_internal::try_allocate_objects" ||
            info->qualifiedName == "gti_internal::raw_buffer::try_allocate")
          std::cerr << lang::MirPrinter().print(function.body);
      }
    }
  }
  expect(result.canGenerateCode(),
         "growable insertion retains a dynamic text owner");
  const auto *push = namedFunction(result, "std::vector::push_back");
  expect(push != nullptr, "growable insertion has a source body");
  if (push) {
    const auto effects =
        lang::deriveMirRetainingStoreEffects(*push, &result.mir);
    expect(effects.complete,
           "growable insertion publishes complete retaining effects");
  }
}

// Issue #379: storing a view into a field names what the view borrows. A
// temporary owner dies at the end of the statement; a named owner is local.
// Both suggest an owning std::string field.
void testStoredViewOwnerDiagnostics() {
  const std::string declarations = R"gti(
import std.filesystem;
struct Metadata {
  Metadata() {}
  mut std::string_view name = "";
};
)gti";
  const auto diagnosticWith = [](const lang::FrontendResult &result,
                                 std::string_view text) {
    return std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
                        [&](const lang::Diagnostic &diagnostic) {
                          return diagnostic.code == "GTI-S2045" &&
                                 diagnostic.message.find(text) !=
                                     std::string::npos;
                        });
  };
  const auto temporary = analyzeText(declarations + R"gti(
int main() {
  std::filesystem::path entry = std::filesystem::path("dir/file.txt");
  mut Metadata data{};
  data.name = entry.filename().view();
  return 0;
}
)gti");
  const auto fromTemporary =
      diagnosticWith(temporary, "borrows a temporary that is destroyed");
  expect(
      fromTemporary != temporary.diagnostics.end() &&
          std::any_of(
              fromTemporary->related.begin(), fromTemporary->related.end(),
              [](const auto &related) {
                return related.message.find("This temporary owns the text") !=
                       std::string::npos;
              }) &&
          std::any_of(fromTemporary->hints.begin(), fromTemporary->hints.end(),
                      [](const std::string &hint) {
                        return hint.find("owning std::string") !=
                               std::string::npos;
                      }),
      "#379 a view of a temporary owner names the temporary");
  const auto named = analyzeText(declarations + R"gti(
int main() {
  std::filesystem::path entry = std::filesystem::path("dir/file.txt");
  mut Metadata data{};
  data.name = entry.view();
  return 0;
}
)gti");
  const auto fromNamed = diagnosticWith(named, "borrows 'entry'");
  expect(fromNamed != named.diagnostics.end() &&
             fromNamed->message.find("temporary") == std::string::npos &&
             fromNamed->message.find("single-owner") == std::string::npos,
         "#379 a view of a named local owner names that owner");
}

// Issue #392: a call on a temporary receiver whose result publishes a text
// argument in a field (L-OWN-02) borrows that argument's owner, not the
// temporary; chaining or binding the result is accepted, and a temporary text
// owner is still rejected.
// Issue #393: an operator call that forwards through `this[...]` takes its
// receiver directly; it forwards to that overload's receiver contract like a
// named helper call does, rather than requiring every receiver text field.
void testTemporaryReceiverTextPublication() {
  const std::string classes = R"gti(
import std.string;
import std.vector;
class Result {
  std::string_view subject;
  std::vector<std::size_t> spans;
public:
  Result(std::string_view subject, std::vector<std::size_t> spans)
      : subject(subject), spans(std::move(spans)) {}
  std::size_t length() { return this.spans.size(); }
  std::size_t operator[](std::size_t n) { return this.spans[n]; }
  std::size_t operator[](std::string_view name) { return this[std::size_t(0)]; }
};
class Finder {
  std::vector<int> program;
public:
  Finder(int seed) : program(std::vector<int>()) {}
  Result find(std::string_view subject) {
    mut std::vector<std::size_t> spans{};
    spans.push_back(1);
    return Result(subject, std::move(spans));
  }
};
)gti";
  for (const auto &[name, body] :
       {std::pair{"#392 chained", "std::size_t n = "
                                  "Finder(1).find(text.view()).length();"},
        std::pair{"#392 bound", "Result r = Finder(1).find(text.view()); "
                                "std::size_t n = r.length();"},
        std::pair{"#393 forwarding subscript",
                  "Result r = Finder(1).find(text.view()); "
                  "std::size_t n = r[\"x\"];"}}) {
    const auto result = analyzeText(classes +
                                    "int main() {\n"
                                    "  std::string text{\"abc\"};\n  " +
                                    body + "\n  return int32_t(n) - 1;\n}\n");
    if (!result.canGenerateCode())
      printDiagnostics(result);
    expect(result.canGenerateCode() &&
               lang::verifyMirProgram(result.mir).valid(),
           std::string(name) + " is accepted with valid MIR");
  }
  const auto dying = analyzeText(
      classes + "int main() {\n"
                "  Result r = Finder(1).find(std::string(\"xyz\").view());\n"
                "  return int32_t(r.length());\n}\n");
  expect(!dying.canGenerateCode() &&
             hasDiagnosticCode(dying.diagnostics, "GTI-S2045") &&
             !hasDiagnosticCode(dying.diagnostics, "GTI-B0001"),
         "#392 a temporary text owner published through a temporary receiver "
         "is still rejected");
}

constexpr NamedTest kTests[] = {
    {"testGrowableBorrowedRecordReturn", testGrowableBorrowedRecordReturn},
    {"testCopiedReferenceParameterProof", testCopiedReferenceParameterProof},
    {"testOwnedConstructionHir", testOwnedConstructionHir},
    {"testOwnedMutableBorrowHir", testOwnedMutableBorrowHir},
    {"testOwnedEmplaceRecordRead", testOwnedEmplaceRecordRead},
    {"testDynamicTextCleanupCapacity", testDynamicTextCleanupCapacity},
    {"testBorrowedArrayCleanupContainment",
     testBorrowedArrayCleanupContainment},
    {"testOwnedTextDestructorBoundary", testOwnedTextDestructorBoundary},
    {"testOwnedTextConstructorEscapeBoundary",
     testOwnedTextConstructorEscapeBoundary},
    {"testTextFieldInitializerStage", testTextFieldInitializerStage},
    {"testOwnedRecordCopyReleasesStorage", testOwnedRecordCopyReleasesStorage},
    {"testOwnedConstructedTextFieldRead", testOwnedConstructedTextFieldRead},
    {"testOwnedRecordElementReplacementRead",
     testOwnedRecordElementReplacementRead},
    {"testOwnedElementReplacementRead", testOwnedElementReplacementRead},
    {"testOwnedConstructionMutableRecordRead",
     testOwnedConstructionMutableRecordRead},
    {"testOwnedConstructionMir", testOwnedConstructionMir},
    {"testOwnedConstructionRecordRead", testOwnedConstructionRecordRead},
    {"testOwnedConstructionPairRead", testOwnedConstructionPairRead},
    {"testOwnedConstructionViewRead", testOwnedConstructionViewRead},
    {"testGrowableBorrowedClone", testGrowableBorrowedClone},
    {"testGrowableBorrowedRecordClone", testGrowableBorrowedRecordClone},
    {"testGrowableBorrowedCloneRetention", testGrowableBorrowedCloneRetention},
    {"testGrowableBorrowedRecordAccess", testGrowableBorrowedRecordAccess},
    {"testReturnedTextRetention", testReturnedTextRetention},
    {"testNestedLifecycleRetention", testNestedLifecycleRetention},
    {"testReturnedAggregateRetention", testReturnedAggregateRetention},
    {"testFailureSnapshotRetention", testFailureSnapshotRetention},
    {"testOpaqueOwnerUnknownRetention", testOpaqueOwnerUnknownRetention},
    {"testSymbolicAggregateRetention", testSymbolicAggregateRetention},
    {"testRetainingExplicitCleanup", testRetainingExplicitCleanup},
    {"testRetainingLocalCleanup", testRetainingLocalCleanup},
    {"testProcessAllocatorRetention", testProcessAllocatorRetention},
    {"testRetainingLocalConstruction", testRetainingLocalConstruction},
    {"testGrowableBorrowedRetention", testGrowableBorrowedRetention},
    {"testOwnedStorageReceiverRead", testOwnedStorageReceiverRead},
    {"testOwnedStorageReceiverWrite", testOwnedStorageReceiverWrite},
    {"testOwnedStorageMultipleOwners", testOwnedStorageMultipleOwners},
    {"testOwnedStorageMultipleRecords", testOwnedStorageMultipleRecords},
    {"testOwnedStorageReferenceReceiver", testOwnedStorageReferenceReceiver},
    {"testOwnedStorageReadWrapper", testOwnedStorageReadWrapper},
    {"testOwnedStorageBorrowedRecord", testOwnedStorageBorrowedRecord},
    {"testOwnedStorageReferenceArgument", testOwnedStorageReferenceArgument},
    {"testOwnedStorageBorrowedRead", testOwnedStorageBorrowedRead},
    {"testOwnedStorageInitialization", testOwnedStorageInitialization},
    {"testArrayResultLiteralReturnProof", testArrayResultLiteralReturnProof},
    {"testArrayResultLifetimeSuppliers", testArrayResultLifetimeSuppliers},
    {"testInlineArrayCallSupplierSet", testInlineArrayCallSupplierSet},
    {"testReferenceArrayExternalEntry", testReferenceArrayExternalEntry},
    {"testExpiredStoredReturn", testExpiredStoredReturn},
    {"testExplicitEmptyCarrierProof", testExplicitEmptyCarrierProof},
    {"testJoinedCarrierCannotSelectOneOrigin",
     testJoinedCarrierCannotSelectOneOrigin},
    {"testConditionalStoredValueFlow", testConditionalStoredValueFlow},
    {"testInferredArrayReturnSuppliers", testInferredArrayReturnSuppliers},
    {"testArrayPublicationDiagnostic", testArrayPublicationDiagnostic},
    {"testArrayOwnerlessPublicationContract",
     testArrayOwnerlessPublicationContract},
    {"testBorrowedArrayProjectionProof", testBorrowedArrayProjectionProof},
    {"testGenericArrayReceiverRetention", testGenericArrayReceiverRetention},
    {"testGenericFieldReceiverRetention", testGenericFieldReceiverRetention},
    {"testRetainingStoreEffectsOnLoweredProgram",
     testRetainingStoreEffectsOnLoweredProgram},
    {"testRetainingStoreContractSeparation",
     testRetainingStoreContractSeparation},
    {"testRetainingCallableEscape", testRetainingCallableEscape},
    {"testRetainingNamedCallableEscape", testRetainingNamedCallableEscape},
    {"testOpaqueRetainingLambdaEscape", testOpaqueRetainingLambdaEscape},
    {"testOpaqueRetainingDirectEscape", testOpaqueRetainingDirectEscape},
    {"testCapturedRetainingStoreEscape", testCapturedRetainingStoreEscape},
    {"testRetainingCallConditionalCleanupProof",
     testRetainingCallConditionalCleanupProof},
    {"testGenericArrayElementReturnContract",
     testGenericArrayElementReturnContract},
    {"testGenericArrayElementReturnProof", testGenericArrayElementReturnProof},
    {"testCarrierFlowOnLoweredProgram", testCarrierFlowOnLoweredProgram},
    {"testReplacementDependencyVerification",
     testReplacementDependencyVerification},
    {"testMultipleTextOwners", testMultipleTextOwners},
    {"testTemporaryReceiverTextPublication",
     testTemporaryReceiverTextPublication},
    {"testStoredViewOwnerDiagnostics", testStoredViewOwnerDiagnostics},
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
