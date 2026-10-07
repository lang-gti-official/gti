#include "compiler_test_fixture.h"

namespace {

void testSemanticVisitorRequestReuse() {
  const auto parseProgram = [](std::string source) {
    lang::Lexer lexer;
    std::vector<lang::Token> tokens = lexer.scan(std::move(source));
    expect(!lexer.hadError(), "semantic reuse fixture should lex");
    lang::Parser parser(std::move(tokens));
    lang::Program program = parser.parse();
    expect(!parser.hadError(), "semantic reuse fixture should parse");
    return program;
  };
  const auto parseExpression = [](std::string source) {
    lang::Lexer lexer;
    std::vector<lang::Token> tokens = lexer.scan(std::move(source));
    expect(!lexer.hadError(), "semantic reuse expression should lex");
    lang::Parser parser(std::move(tokens));
    lang::ExprPtr expression = parser.parseExpression();
    expect(!parser.hadError() && expression != nullptr,
           "semantic reuse expression should parse");
    return expression;
  };

  lang::Program first = parseProgram(R"(
static int hidden = 7;
int first(mut int& value) {
  mut int& alias = value;
  return alias;
}
)");
  const lang::FunctionDecl *firstFunction =
      findTopLevelFunction(first, "first");
  const auto *firstReturn =
      firstFunction != nullptr && firstFunction->body() != nullptr
          ? dynamic_cast<const lang::ReturnStmt *>(
                firstFunction->body()->statements().back().get())
          : nullptr;
  const lang::Expr *firstReturnExpression =
      firstReturn != nullptr ? firstReturn->value().get() : nullptr;

  lang::SemanticVisitor semantic;
  expect(semantic.check(first),
         "the first request in a reusable semantic visitor should pass");
  const lang::FunctionInfo *firstInfo =
      firstFunction != nullptr ? semantic.model().findFunction(*firstFunction)
                               : nullptr;
  expect(firstInfo != nullptr && firstInfo->id == 1,
         "the first request should allocate its first function identity");
  expect(!semantic.model().loans().empty(),
         "the first request should publish its reference loan");
  expect(firstReturnExpression != nullptr &&
             semantic.model().findExpression(*firstReturnExpression) != nullptr,
         "the first request should publish AST-backed expression facts");

  lang::Program second = parseProgram(R"(
int second() { return 2; }
)");
  const lang::FunctionDecl *secondFunction =
      findTopLevelFunction(second, "second");
  const auto *secondReturn =
      secondFunction != nullptr && secondFunction->body() != nullptr
          ? dynamic_cast<const lang::ReturnStmt *>(
                secondFunction->body()->statements().back().get())
          : nullptr;
  expect(semantic.check(second),
         "a second program request should start from a clean session");
  const lang::FunctionInfo *secondInfo =
      secondFunction != nullptr ? semantic.model().findFunction(*secondFunction)
                                : nullptr;
  expect(secondInfo != nullptr && secondInfo->id == 1,
         "function identities should restart for every request");
  expect(firstFunction != nullptr &&
             semantic.model().findFunction(*firstFunction) == nullptr,
         "declaration pointers from the first request must not survive");
  expect(firstReturnExpression != nullptr &&
             semantic.model().findExpression(*firstReturnExpression) == nullptr,
         "expression pointers from the first request must not survive");
  expect(secondReturn != nullptr && secondReturn->value() != nullptr &&
             semantic.model().findExpression(*secondReturn->value()) != nullptr,
         "the second request should publish only its own expression facts");
  expect(semantic.model().loans().empty(),
         "loan-flow state from the first request must not survive");

  lang::Program invalid = parseProgram(R"(
int broken() { return missing; }
)");
  expect(!semantic.check(invalid) && !semantic.errors().empty(),
         "an invalid request should publish diagnostics");
  expect(semantic.check(second) && semantic.errors().empty(),
         "a later valid request should clear earlier diagnostics");

  expect(semantic.check(first),
         "the static-name fixture should remain reusable");
  lang::ExprPtr hidden = parseExpression("hidden");
  expect(hidden != nullptr && !semantic.check(*hidden) &&
             !semantic.errors().empty() &&
             semantic.expressionType() == lang::SemanticType::Unknown,
         "program-to-expression reuse must not retain internal names or the "
         "previous expression/return context");

  lang::ExprPtr boolean = parseExpression("true");
  expect(boolean != nullptr && semantic.check(*boolean) &&
             semantic.errors().empty() && semantic.model().loans().empty() &&
             semantic.expressionType() == lang::SemanticType::Bool,
         "a later expression request should clear diagnostics and flow state "
         "while publishing its own exact type");
}

void testSemanticDiagnostics() {
  lang::Lexer lexer;
  auto tokens = lexer.scan(R"(
int main() {
  if (1) { missing = 3; }
  return 0;
}
)");
  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "semantic test source should parse");

  lang::SemanticVisitor semantic;
  expect(!semantic.check(program), "invalid semantics should be rejected");
  expect(semantic.errors().size() == 2,
         "condition type and undefined variable should both be reported");
  expect(!semantic.errors().empty() &&
             semantic.errors().front().primary.line == 3,
         "semantic diagnostics should preserve literal source lines");
}

void testOwnershipAndMissingIncludeDiagnostics() {
  const auto analyze = [](const std::string &source) {
    return lang::Frontend().analyze("ownership-explanations.gti", source,
                                    {standardLibraryPrelude()}, {},
                                    {standardLibraryRoot()});
  };
  const std::string prefix = "#include <std/string>\n";
  const std::string moved = prefix + R"(
int main() {
  std::string original = std::string("hello");
  std::string destination = std::move(original);
  return int(original.size());
})";
  const auto movedResult = analyze(moved);
  const auto *movedError =
      findDiagnosticByCode(movedResult.diagnostics, "GTI-S2018");
  expect(movedError && movedError->primary.start == moved.rfind("original") &&
             movedError->related.size() == 1 &&
             movedError->related.front().span.start ==
                 moved.find("original);") &&
             movedError->related.front().span.end ==
                 moved.find("original);") + 8 &&
             movedError->related.front().message ==
                 "Ownership transferred here." &&
             !movedError->hints.empty() && movedError->fixes.empty(),
         "use-after-move must explain the actual transfer without guessing a "
         "repair");
  const std::string branched = prefix + R"(
void examine(bool condition) {
  mut std::string original = std::string("hello");
  if (condition) { std::string first = std::move(original); }
  else { std::string second = std::move(original); }
  std::println(original.view());
})";
  const auto branchResult = analyze(branched);
  const auto *branchError =
      findDiagnosticByCode(branchResult.diagnostics, "GTI-S2018");
  expect(branchError && branchError->related.size() == 2 &&
             branchError->related[0].span.start ==
                 branched.find("original);") &&
             branchError->related[1].span.start == branched.rfind("original);"),
         "control-flow joins preserve both transfer causes in source order");
  auto reinitialized = branched;
  reinitialized.insert(reinitialized.find("  std::println"),
                       "  original = std::string(\"replacement\");\n");
  expect(
      analyze(reinitialized).canGenerateCode(),
      "diagnostic provenance must not change valid reinitialization semantics");
  reinitialized.insert(reinitialized.find("  std::println"),
                       "  std::string third = std::move(original);\n");
  const auto newMove = analyze(reinitialized);
  const auto *newMoveError =
      findDiagnosticByCode(newMove.diagnostics, "GTI-S2018");
  expect(newMoveError && newMoveError->related.size() == 1 &&
             newMoveError->related[0].span.start ==
                 reinitialized.rfind("original);"),
         "reinitialization clears old move causes before a new transfer");
  const std::string borrowed = prefix + R"(
int main() {
  mut std::string owner = std::string("hello");
  auto view = owner.view();
  owner.clear();
  return int(view.size());
})";
  const auto borrowResult = analyze(borrowed);
  expect(hasDiagnosticCode(borrowResult.diagnostics, "GTI-S2017") &&
             hasRelatedDiagnostic(borrowResult.diagnostics,
                                  "borrow originates here") &&
             hasRelatedDiagnostic(borrowResult.diagnostics, "used later") &&
             hasDiagnosticHint(borrowResult.diagnostics, "Finish using"),
         "retained-borrow conflicts identify their origin and later use");
  const auto *borrowError =
      findDiagnosticByCode(borrowResult.diagnostics, "GTI-S2017");
  expect(borrowError && borrowError->related.size() == 2 &&
             borrowError->related[1].span.start ==
                 borrowed.find("return int") &&
             borrowError->related[1].span.end ==
                 borrowed.find(";\n}", borrowed.find("return int")) + 1 &&
             borrowError->fixes.empty(),
         "the later-use explanation spans the actual statement without an "
         "effect-reordering fix");
  const auto endedBorrow = analyze(prefix + R"(
int main() {
  mut std::string owner = std::string("hello");
  auto view = owner.view();
  int result = int(view.size());
  owner.clear();
  return result;
})");
  expect(endedBorrow.canGenerateCode(),
         "diagnostic provenance must not extend a borrow beyond its last use");

  const std::string records = R"(
struct Ticket {
  mut int code = 7;
  Ticket() = default;
  Ticket(Ticket& other) = delete;
  Ticket(Ticket&& other) = default;
};
struct Holder { mut Ticket field = Ticket(); };
)";
  const std::string projected = records + R"(
void examine(bool condition) {
  mut Holder holder{};
  if (condition) { Ticket first = std::move(holder.field); }
  else { Ticket second = std::move(holder.field); }
  int code = holder.field.code;
})";
  const auto projectedResult = analyze(projected);
  const auto *projectedError =
      findDiagnosticByCode(projectedResult.diagnostics, "GTI-S2018");
  expect(
      projectedError && projectedError->related.size() == 2 &&
          projectedError->related[0].span.start == projected.find("field);") &&
          projectedError->related[1].span.start == projected.rfind("field);") &&
          projectedError->related[0].span.end == projected.find("field);") + 5,
      "projected moves of a user-defined owner retain both exact field "
      "transfer sites");
  auto restoredField = projected;
  restoredField.insert(restoredField.find("  int code"),
                       "  holder.field = Ticket();\n");
  expect(analyze(restoredField).canGenerateCode(),
         "field restoration clears diagnostic provenance with the moved state");
  restoredField.insert(restoredField.find("  int code"),
                       "  Ticket third = std::move(holder.field);\n");
  const auto fieldMovedAgain = analyze(restoredField);
  const auto *fieldMoveError =
      findDiagnosticByCode(fieldMovedAgain.diagnostics, "GTI-S2018");
  expect(
      fieldMoveError && fieldMoveError->related.size() == 1 &&
          fieldMoveError->related[0].span.start ==
              restoredField.rfind("field);"),
      "a restored field reports only the new transfer, not old branch sites");
  const auto shadowed = analyze(records + R"(
void examine() {
  Ticket original{};
  { Ticket original{}; Ticket destination = std::move(original); }
  int code = original.code;
})");
  expect(shadowed.canGenerateCode(),
         "move notes follow declaration identity rather than a shadowed name");
  const auto receiverResult = analyze(records + R"(
class Owner {
  mut Ticket field = Ticket();
public:
  void remove() mut { Ticket previous = std::move(this.field); }
};
)");
  expect(hasDiagnosticCode(receiverResult.diagnostics, "GTI-S2018") &&
             hasRelatedDiagnostic(receiverResult.diagnostics,
                                  "Ownership transferred here.") &&
             hasDiagnosticHint(receiverResult.diagnostics,
                               "Reinitialize every moved field"),
         "an unrestored receiver explains its transfer and restoration duty");
  std::string manyPaths =
      records + "\nvoid examine(int path) { Ticket source{};\n";
  for (int path = 0; path < 5; ++path) {
    manyPaths += "if (path == " + std::to_string(path) +
                 ") { Ticket destination = std::move(source); } else ";
  }
  manyPaths +=
      "{ Ticket destination = std::move(source); }\nint code = source.code; }";
  const auto manyResult = analyze(manyPaths);
  const auto *manyError =
      findDiagnosticByCode(manyResult.diagnostics, "GTI-S2018");
  expect(manyError && manyError->related.size() == 4 &&
             std::is_sorted(manyError->related.begin(),
                            manyError->related.end(),
                            [](const auto &left, const auto &right) {
                              return left.span.start < right.span.start;
                            }),
         "move causes are bounded and deterministic across many branch joins");
  lang::FrontendOptions concurrentOptions;
  concurrentOptions.target.executionProfile =
      lang::ExecutionProfile::Concurrent;
  concurrentOptions.target.runtimeCapabilities.threads = true;
  const std::string guarded = R"(
#include <std/mutex>
int main() {
  std::mutex<int32_t> owner{1};
  std::mutex_guard<int32_t> first = owner.lock();
  std::mutex_guard<int32_t> second = owner.lock();
  return 0;
})";
  const auto scopedGuard =
      lang::Frontend(concurrentOptions)
          .analyze("guard-explanations.gti", guarded,
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(
      hasDiagnosticCode(scopedGuard.diagnostics, "GTI-S2017") &&
          hasDiagnosticHint(scopedGuard.diagnostics,
                            "until its carrier is destroyed") &&
          !hasDiagnosticHint(scopedGuard.diagnostics, "Finish using"),
      "a resource-bound loan explains destruction, not a last-read endpoint");
  auto releasedGuard = guarded;
  releasedGuard.insert(releasedGuard.find("  std::mutex_guard<int32_t> first"),
                       "  {\n");
  releasedGuard.insert(releasedGuard.find("  std::mutex_guard<int32_t> second"),
                       "  }\n");
  expect(lang::Frontend(concurrentOptions)
             .analyze("ended-guard-explanations.gti", releasedGuard,
                      {standardLibraryPrelude()}, {}, {standardLibraryRoot()})
             .canGenerateCode(),
         "ending a guard's scope still releases its lifecycle-bound loan");
  const auto globalWrite =
      lang::Frontend(concurrentOptions)
          .analyze("concurrent-global-explanations.gti", R"(
struct Command { mut int name = 0; };
Command cmd{};
int count = 0;
void examine() { cmd.name = 1; count = 1; }
)",
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(hasDiagnosticHint(globalWrite.diagnostics, "must stay immutable") &&
             !hasDiagnosticHint(globalWrite.diagnostics,
                                "Declare 'cmd' with 'mut'") &&
             !hasDiagnosticHint(globalWrite.diagnostics, "add 'mut'"),
         "concurrent global mutation advice must preserve the "
         "immutable-storage policy");
  const std::string readonly = R"(
struct Command { mut int name = 0; };
int main() {
  Command cmd{};
  cmd.name = 1;
  return 0;
})";
  const auto readonlyResult = analyze(readonly);
  expect(hasRelatedDiagnostic(readonlyResult.diagnostics,
                              "'cmd' is declared without mutable access") &&
             hasDiagnosticHint(readonlyResult.diagnostics,
                               "Declare 'cmd' with 'mut'"),
         "mutable fields through immutable values explain the binding "
         "restriction");
  auto mutableSource = readonly;
  mutableSource.insert(mutableSource.find("Command cmd"), "mut ");
  expect(analyze(mutableSource).canGenerateCode(),
         "an explicitly mutable owner still accepts field writes");
  const auto referenceResult = analyze(R"(
struct Command { mut int name = 0; };
void update(Command& cmd) { cmd.name = 1; }
)");
  expect(hasRelatedDiagnostic(referenceResult.diagnostics,
                              "read-only reference") &&
             hasDiagnosticHint(referenceResult.diagnostics, "exclusive access"),
         "read-only reference advice must explain the access contract, not "
         "only binding mutability");
  const auto arrayWrite =
      analyze("void examine() { int values[2] = {1, 2}; values[0] = 3; }");
  expect(hasRelatedDiagnostic(arrayWrite.diagnostics,
                              "'values' is declared without mutable access") &&
             hasDiagnosticHint(arrayWrite.diagnostics,
                               "Declare 'values' with 'mut'"),
         "a built-in fixed-array write identifies its immutable binding");
  const auto capturedWrite = analyze(R"(
struct Command { mut int name = 0; };
void examine() {
  mut Command cmd{};
  auto modify = [cmd]() -> void { cmd.name = 1; };
})");
  expect(hasDiagnosticHint(capturedWrite.diagnostics,
                           "immutable value snapshot") &&
             !hasDiagnosticHint(capturedWrite.diagnostics,
                                "Declare 'cmd' with 'mut'"),
         "capture mutation advice must not suggest changing the mutable source "
         "binding");
  const auto immutableMethod = analyze(R"(
struct Command { void update() mut {} };
void examine() { Command cmd{}; cmd.update(); }
)");
  expect(hasRelatedDiagnostic(immutableMethod.diagnostics,
                              "'cmd' is declared without mutable access") &&
             hasRelatedDiagnostic(immutableMethod.diagnostics,
                                  "requires mutable receiver access") &&
             hasDiagnosticHint(immutableMethod.diagnostics, "Declare 'cmd'"),
         "mutable method calls explain both binding and callable contracts");
  const auto constantWrite =
      analyze("void examine() { constexpr int fixed = 4; fixed = 5; }");
  expect(hasDiagnosticHint(constantWrite.diagnostics,
                           "A constant cannot be mutated") &&
             !hasDiagnosticHint(constantWrite.diagnostics, "add 'mut'"),
         "constexpr mutation advice must not recommend a forbidden mut prefix");
  const std::string nonassignable = R"(
class Slot {
  mut int& value;
public:
  Slot(mut int& owner) : value(owner) {}
};
void examine() {
  mut int first = 1;
  mut int second = 2;
  mut Slot destination = Slot(first);
  Slot source = Slot(second);
  destination = source;
})";
  const auto assignmentResult = analyze(nonassignable);
  const auto *assignmentError =
      findDiagnosticByCode(assignmentResult.diagnostics, "GTI-S2003");
  expect(assignmentError &&
             assignmentError->message.find("Cannot copy assign") !=
                 std::string::npos &&
             assignmentError->primary.start == nonassignable.rfind("source;") &&
             hasDiagnosticHint(assignmentResult.diagnostics,
                               "does not support move assignment") &&
             !hasDiagnosticHint(assignmentResult.diagnostics,
                                "use std::move(owner)") &&
             assignmentError->fixes.empty(),
         "copy failure must not recommend a move assignment the type lacks");
  for (const std::string value : {"std::move(source)", "Slot(second)"}) {
    auto unavailableMove = nonassignable;
    unavailableMove.replace(unavailableMove.rfind("destination = source"),
                            std::string("destination = source").size(),
                            "destination = " + value);
    const auto result = analyze(unavailableMove);
    const auto *error = findDiagnosticByCode(result.diagnostics, "GTI-S2003");
    expect(error &&
               error->message.find("Move assignment is unavailable") !=
                   std::string::npos &&
               error->fixes.empty() &&
               !hasDiagnosticHint(result.diagnostics, "use std::move(owner)"),
           "explicit movement and fresh values both explain unavailable move "
           "assignment");
  }

  const std::string dereferencePrefix = R"(
#include <std/string>
class Slot {
  mut std::string value = std::string("old");
public:
  mut std::string& operator*() mut { return value; }
};
int main() {
  mut Slot slot{};
  std::string source = std::string("new");
  *slot = )";
  const std::string copied = dereferencePrefix + "source; return 0; }";
  const auto copyResult = lang::Frontend().analyze(
      "dereference-owner-copy.gti", copied, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  const auto *copyError =
      findDiagnosticByCode(copyResult.diagnostics, "GTI-S2003");
  expect(
      !copyResult.canGenerateCode() && copyError != nullptr &&
          copyError->phase == lang::DiagnosticPhase::Semantics &&
          copyError->primary.source ==
              std::filesystem::weakly_canonical("dereference-owner-copy.gti")
                  .string() &&
          copyError->primary.start == dereferencePrefix.size() &&
          copyError->primary.end == dereferencePrefix.size() + 6 &&
          copyError->message.find("Cannot copy assign") != std::string::npos &&
          !copyError->hints.empty() && copyError->fixes.empty() &&
          !hasDiagnosticCode(copyResult.diagnostics, "GTI-B0001"),
      "user-defined dereference assignment must reject implicit owner copies "
      "at the RHS before lowering, without an ownership-changing fix-it");
  for (const std::string &value : {std::string("std::move(source)"),
                                   std::string("std::string(\"fresh\")")}) {
    const auto valid = lang::Frontend().analyze(
        "dereference-owner-move.gti",
        dereferencePrefix + value + "; return 0; }", {standardLibraryPrelude()},
        {}, {standardLibraryRoot()});
    expect(valid.canGenerateCode() && lang::verifyMirProgram(valid.mir).valid(),
           "owned RHS values should retain valid dereference move assignment");
  }

  const std::string ownershipSource = R"(
#include <std/string>
#include <std/vector>

class Options {
  std::vector<std::string> arguments;
public:
  Options(std::vector<std::string> argv) : arguments(argv) {}
};
)";
  const lang::FrontendResult ownership = lang::Frontend().analyze(
      "move-only-field-initializer.gti", ownershipSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const lang::Diagnostic *copy =
      findDiagnosticByCode(ownership.diagnostics, "GTI-S2003");
  const std::size_t copiedArgument = ownershipSource.find("arguments(argv)") +
                                     std::string("arguments(").size();
  expect(!ownership.canGenerateCode() && copy != nullptr &&
             copy->primary.start == copiedArgument &&
             copy->primary.end == copiedArgument + 4 &&
             copy->message ==
                 "Cannot initialize field 'arguments' by copying move-only "
                 "type 'std::vector<std::string>'." &&
             copy->hints ==
                 std::vector<std::string>{
                     "Transfer ownership explicitly with "
                     "std::move(argv), or initialize the field "
                     "from a fresh value."} &&
             copy->fixes.empty(),
         "move-only field initialization should explain the ownership cause "
         "without offering an intent-changing edit");

  const std::string returnSource = R"(
#include <std/string>

std::string return_named(std::string result) {
  return result;
}
)";
  const lang::FrontendResult returned = lang::Frontend().analyze(
      "move-only-return.gti", returnSource, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  const lang::Diagnostic *returnCopy =
      findDiagnosticByCode(returned.diagnostics, "GTI-S2003");
  expect(returned.canGenerateCode() && returnCopy == nullptr,
         "an exact named move-only return should move implicitly without "
         "publishing the former copy diagnostic");

  const std::string expectedReturnSource = R"(
#include <std/string>
enum class error { failed };
expected<std::string, error> value() {
  std::string text = std::string("tea");
  return text;
}
int main() { return 0; }
)";
  const lang::FrontendResult expectedReturned = lang::Frontend().analyze(
      "move-only-expected-return.gti", expectedReturnSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const lang::Diagnostic *expectedCopy =
      findDiagnosticByCode(expectedReturned.diagnostics, "GTI-S2003");
  const std::size_t expectedName =
      expectedReturnSource.find("return text") + std::string("return ").size();
  expect(!expectedReturned.canGenerateCode() && expectedCopy != nullptr &&
             expectedCopy->primary.source ==
                 std::filesystem::absolute("move-only-expected-return.gti")
                     .lexically_normal()
                     .string() &&
             expectedCopy->primary.start == expectedName &&
             expectedCopy->primary.end == expectedName + 4 &&
             expectedCopy->message ==
                 "Cannot return 'text' by copy because type 'std::string' "
                 "is move-only." &&
             expectedCopy->hints ==
                 std::vector<std::string>{"Use 'return std::move(text);' to "
                                          "transfer ownership."} &&
             expectedCopy->fixes.empty(),
         "wrapping a named success payload must diagnose its attempted copy "
         "before backend admission, with the same exact ownership hint");
  std::string nestedReturnSource = expectedReturnSource;
  nestedReturnSource.replace(
      nestedReturnSource.find("expected<std::string, error>"),
      std::string("expected<std::string, error>").size(),
      "expected<expected<std::string, error>, error>");
  const auto nestedReturned = lang::Frontend().analyze(
      "nested-expected-return.gti", nestedReturnSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const auto *nestedCopy =
      findDiagnosticByCode(nestedReturned.diagnostics, "GTI-S2003");
  expect(!nestedReturned.canGenerateCode() && nestedCopy != nullptr &&
             nestedCopy->message == expectedCopy->message &&
             nestedCopy->hints == expectedCopy->hints &&
             nestedCopy->primary.start ==
                 nestedReturnSource.find("return text") +
                     std::string("return ").size(),
         "nested expected success conversions cannot bypass payload ownership");
  for (const std::string replacement :
       {"std::move(text)", "std::string(\"tea\")"}) {
    std::string valid = expectedReturnSource;
    valid.replace(expectedName, 4, replacement);
    const lang::FrontendResult accepted = lang::Frontend().analyze(
        "owned-expected-success.gti", valid, {standardLibraryPrelude()}, {},
        {standardLibraryRoot()});
    expect(accepted.canGenerateCode(),
           "explicitly moved and fresh success payloads remain legal");
  }

  const std::string missingSource = "using values = std::vector<int>;\n";
  const lang::FrontendResult missing = lang::Frontend().analyze(
      "missing-vector-include.gti", missingSource, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  const lang::Diagnostic *include =
      findDiagnosticByCode(missing.diagnostics, "GTI-S2089");
  expect(!missing.semanticValid && include != nullptr &&
             include->primary.start == missingSource.find("vector") &&
             include->primary.end ==
                 missingSource.find("vector") + std::string("vector").size() &&
             include->message == "Unknown type 'std::vector'." &&
             hasDiagnosticHint(missing.diagnostics, "#include <std/vector>") &&
             include->fixes.size() == 1 &&
             include->fixes.front().span.source == include->primary.source &&
             include->fixes.front().span.start == 0 &&
             include->fixes.front().span.end == 0 &&
             include->fixes.front().replacement == "#include <std/vector>\n" &&
             include->fixes.front().message == "Add #include <std/vector>",
         "an unresolved canonical standard-library type should carry one "
         "compiler-owned include fix");

  for (const std::string name : {"function", "move_only_function"}) {
    const std::string source = "using callback = std::" + name + "<void()>;\n";
    const lang::FrontendResult callback = lang::Frontend().analyze(
        "missing-callback-include.gti", source, {standardLibraryPrelude()}, {},
        {standardLibraryRoot()});
    const lang::Diagnostic *missingCallback =
        findDiagnosticByCode(callback.diagnostics, "GTI-S2089");
    expect(
        !callback.canGenerateCode() && missingCallback != nullptr &&
            missingCallback->primary.start == source.find(name) &&
            missingCallback->primary.end == source.find(name) + name.size() &&
            missingCallback->fixes.size() == 1 &&
            missingCallback->fixes.front().replacement ==
                "#include <std/functional>\n" &&
            missingCallback->fixes.front().span.start == 0 &&
            missingCallback->fixes.front().span.end == 0,
        "owning callback types should offer the canonical functional include");
    const lang::FrontendResult corrected = lang::Frontend().analyze(
        "missing-callback-include.gti", "#include <std/functional>\n" + source,
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(
        corrected.canGenerateCode(),
        "applying the callback include fix should resolve the signature type");
  }

  for (const std::string name :
       {"map", "unordered_map", "set", "unordered_set", "deque", "queue"}) {
    const std::string source =
        "using dictionary = std::" + name +
        (name == "map" || name == "unordered_map" ? "<int, int>;\n"
                                                  : "<int>;\n");
    const auto missingMap = lang::Frontend().analyze(
        "missing-associative-include.gti", source, {standardLibraryPrelude()},
        {}, {standardLibraryRoot()});
    const lang::Diagnostic *diagnostic =
        findDiagnosticByCode(missingMap.diagnostics, "GTI-S2089");
    const std::string directive = "#include <std/" + name + ">\n";
    expect(!missingMap.canGenerateCode() && diagnostic != nullptr &&
               diagnostic->primary.start == source.find(name) &&
               diagnostic->primary.end == source.find(name) + name.size() &&
               diagnostic->fixes.size() == 1 &&
               diagnostic->fixes.front().span.source ==
                   diagnostic->primary.source &&
               diagnostic->fixes.front().span.start == 0 &&
               diagnostic->fixes.front().span.end == 0 &&
               diagnostic->fixes.front().replacement == directive,
           "associative containers should offer the canonical include fix");
    const auto corrected = lang::Frontend().analyze(
        "missing-associative-include.gti", directive + source,
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(corrected.canGenerateCode(),
           "applying an associative include fix should resolve the type");
  }

  const std::string compoundBorrow = R"(
#include <std/unordered_set>
int main() {
  mut std::unordered_set<int> values{};
  int key = 7;
  if (values.insert(7).second || !values.contains(key)) { return 1; }
  return 0;
}
)";
  const auto compound = lang::Frontend().analyze(
      "compound-result-loan.gti", compoundBorrow, {standardLibraryPrelude()},
      {}, {standardLibraryRoot()});
  const auto *conflict =
      findDiagnosticByCode(compound.diagnostics, "GTI-S2017");
  expect(!compound.semanticValid && conflict != nullptr &&
             conflict->primary.start == compoundBorrow.find("||") &&
             conflict->primary.end == compoundBorrow.find("||") + 2 &&
             !conflict->related.empty() && !conflict->hints.empty() &&
             conflict->fixes.empty() &&
             !hasDiagnosticCode(compound.diagnostics, "GTI-B0001"),
         "a copied field of a temporary borrowed carrier keeps its loan until "
         "the full expression ends and receives a source diagnostic");

  const std::string scalarReads = R"(
#include <std/string>
int main() {
  mut std::string text = std::string("ab");
  text.push_back('c');
  return text[0] != text[1] && text[1] != text[2] ? 0 : 1;
}
)";
  const auto reads = lang::Frontend().analyze(
      "scalar-reference-reads.gti", scalarReads, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(reads.canGenerateCode(), "copied scalar reads must not acquire a "
                                  "carrier's full-expression lifetime");

  const std::string userCarrier = R"(
class Lease {
  mut int& value;
public:
  bool success = true;
  Lease(mut int& value) : value(value) {}
};
Lease acquire(mut int& value) { return Lease(value); }
int main() {
  mut int value = 7;
  if (acquire(value).success && value == 7) { return 1; }
  return 0;
}
)";
  const auto userConflict = lang::Frontend().analyze(
      "user-carrier-operands.gti", userCarrier, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  const auto *userDiagnostic =
      findDiagnosticByCode(userConflict.diagnostics, "GTI-S2017");
  expect(!userConflict.semanticValid && userDiagnostic != nullptr &&
             userDiagnostic->primary.source ==
                 std::filesystem::weakly_canonical("user-carrier-operands.gti")
                     .string() &&
             userDiagnostic->primary.start == userCarrier.find("&&") &&
             userDiagnostic->primary.end == userCarrier.find("&&") + 2 &&
             !userDiagnostic->related.empty() &&
             !userDiagnostic->hints.empty() && userDiagnostic->fixes.empty(),
         "full-expression carrier exclusion is structural, not a container "
         "name exception");

  const lang::FrontendResult indirect = lang::Frontend().analyze(
      "indirect-memory-include.gti",
      "#include <std/cstdio>\nusing owner = std::unique_ptr<int>;\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const lang::Diagnostic *invisible =
      findDiagnosticByCode(indirect.diagnostics, "GTI-S2024");
  expect(!indirect.semanticValid && invisible != nullptr &&
             invisible->fixes.size() == 1 &&
             invisible->fixes.front().span.start == 0 &&
             invisible->fixes.front().span.end == 0 &&
             invisible->fixes.front().replacement ==
                 "#include <std/memory>\n" &&
             invisible->fixes.front().message == "Add #include <std/memory>",
         "a loaded but non-visible standard declaration should carry the "
         "same direct-include fix");

  const lang::FrontendResult unrelated = lang::Frontend().analyze(
      "unknown-vendor-vector.gti", "using values = vendor::vector<int>;\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const lang::Diagnostic *unknown =
      findDiagnosticByCode(unrelated.diagnostics, "GTI-S2000");
  expect(!unrelated.semanticValid && unknown != nullptr &&
             unknown->message == "Unknown type 'vendor::vector'." &&
             unknown->fixes.empty() &&
             !hasDiagnosticCode(unrelated.diagnostics, "GTI-S2089"),
         "unknown non-standard types must not receive a guessed include edit");
}

void testDiagnosticFoundation() {
  for (const auto &[spelling, kind] : lang::keywords) {
    expect(lang::isKeywordToken(kind) || lang::isTypeKeywordToken(kind) ||
               lang::isOperatorToken(kind),
           "compiler token categories should cover keyword spelling '" +
               std::string(spelling) + "'");
  }
  expect(lang::isDirectiveToken(lang::TokenKind::HASH_IF) &&
             lang::isDirectiveToken(lang::TokenKind::HASH_INCLUDE) &&
             !lang::isDirectiveToken(lang::TokenKind::IDENTIFIER),
         "compiler token categories should own the directive range");

  lang::SourceManager sources;
  const std::string unicodePrefix = "\xF0\x9F\x99\x82value";
  sources.set("unicode.gti", unicodePrefix);
  const lang::SourceLocation unicodeLocation =
      sources.locate(lang::SourceSpan{"unicode.gti", 4, 9, 1});
  expect(unicodeLocation.line == 1 && unicodeLocation.column == 2,
         "source locations should count a UTF-8 scalar as one CLI column");

  const std::string multiLine = "first line\nsecond \xCE\xB2 line\nthird\n";
  sources.set("lines.gti", multiLine);
  const lang::SourceLocation secondLine = sources.locate(
      lang::SourceSpan{"lines.gti", multiLine.find("\xCE\xB2"), 0, 1});
  expect(secondLine.line == 2 && secondLine.column == 8 &&
             secondLine.lineStart == multiLine.find("second") &&
             secondLine.lineEnd == multiLine.find("\nthird"),
         "the line index should locate an interior line exactly");
  expect(sources.line(lang::SourceSpan{"lines.gti", multiLine.find("third"), 0,
                                       1}) == "third",
         "line() should return exact line content from the index");
  const lang::SourceLocation atNewline =
      sources.locate(lang::SourceSpan{"lines.gti", multiLine.find('\n'), 0, 1});
  expect(atNewline.line == 1 && atNewline.lineEnd == multiLine.find('\n'),
         "an offset at a line terminator should stay on its own line");
  const lang::SourceLocation pastEnd = sources.locate(lang::SourceSpan{
      "lines.gti", multiLine.size() + 10, multiLine.size() + 11, 1});
  expect(pastEnd.line == 4 && pastEnd.lineStart == multiLine.size(),
         "an out-of-range offset should clamp to the final line");
  const lang::SourceLocation missingSource =
      sources.locate(lang::SourceSpan{"absent.gti", 0, 0, 7});
  expect(missingSource.line == 7,
         "a missing source should fall back to the span line hint");

  lang::Lexer lexer;
  const std::string invalidEscape =
      "std::string_view value = \"first\nbad\\q\";";
  lexer.scan(invalidEscape, "escape.gti");
  expect(
      lexer.errors().size() == 1 &&
          lexer.errors().front().code == "GTI-L0005" &&
          lexer.errors().front().primary.start == invalidEscape.find("\\q") &&
          lexer.errors().front().primary.end == invalidEscape.find("\\q") + 2 &&
          lexer.errors().front().primary.line == 2,
      "lexical diagnostics should identify the exact invalid escape span");

  const std::string missingSemicolon = "int first = 1\nint second = 2;\n";
  lang::Parser parser(lexer.scan(missingSemicolon, "parse.gti"));
  parser.parse();
  expect(parser.errors().size() == 1 &&
             parser.errors().front().code == "GTI-P0001" &&
             parser.errors().front().fixes.size() == 1 &&
             parser.errors().front().fixes.front().replacement == ";" &&
             parser.errors().front().fixes.front().span.start ==
                 missingSemicolon.find("int second"),
         "missing punctuation should carry an insertion fix-it");

  auto semanticTokens = lexer.scan(R"(
int duplicate = 1;
int duplicate = 2;
int main() {
  int fixed = 1;
  fixed = 2;
  int value = "text";
  return 0;
}
)",
                                   "semantic.gti");
  lang::Parser semanticParser(std::move(semanticTokens));
  lang::Program program = semanticParser.parse();
  lang::SemanticVisitor semantic;
  expect(!semantic.check(program),
         "rich semantic diagnostic source should fail");

  const lang::Diagnostic *duplicate = nullptr;
  const lang::Diagnostic *immutable = nullptr;
  const lang::Diagnostic *mismatch = nullptr;
  for (const lang::Diagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.code == "GTI-S2006") {
      duplicate = &diagnostic;
    } else if (diagnostic.code == "GTI-S2002") {
      immutable = &diagnostic;
    } else if (diagnostic.code == "GTI-S2003") {
      mismatch = &diagnostic;
    }
  }
  expect(duplicate != nullptr && duplicate->related.size() == 1,
         "duplicate declarations should reference the original declaration");
  expect(immutable != nullptr && immutable->related.size() == 1 &&
             !immutable->hints.empty(),
         "immutability diagnostics should explain the declaration and remedy");
  expect(mismatch != nullptr &&
             mismatch->message.find("int32_t") != std::string::npos &&
             mismatch->message.find("std::string_view") != std::string::npos,
         "type mismatches should name expected and actual GTI types");
}

void testCppReservedIdentifiers() {
  lang::Lexer lexer;
  for (const std::string_view spelling : lang::cppReservedIdentifiers) {
    const std::vector<lang::Token> tokens =
        lexer.scan(std::string(spelling), "reserved-token.gti");
    expect(tokens.size() == 2 &&
               tokens.front().kind == lang::TokenKind::CPP_RESERVED &&
               lang::isKeywordToken(tokens.front().kind) && !lexer.hadError(),
           "the lexer should reserve C++ keyword spelling '" +
               std::string(spelling) + "'");
  }

  const std::vector<lang::Token> deleteTokens =
      lexer.scan("delete", "delete-token.gti");
  expect(deleteTokens.size() == 2 &&
             deleteTokens.front().kind == lang::TokenKind::DELETE &&
             lang::isKeywordToken(deleteTokens.front().kind),
         "delete should have a dedicated contextual token");

  lang::Parser validDeleteParser(lexer.scan(R"(
class Value {
public:
  Value(Value& other) = delete;
};
)"));
  validDeleteParser.parse();
  expect(!validDeleteParser.hadError(),
         "delete should remain valid in special-member policy");

  const std::string reservedSource = "int template = 1;\n"
                                     "int recovered = 2;\n";
  lang::Parser reservedParser(
      lexer.scan(reservedSource, "reserved-identifier.gti"));
  const lang::Program recovered = reservedParser.parse();
  const lang::Diagnostic *reserved =
      findDiagnosticByCode(reservedParser.errors(), "GTI-P0002");
  expect(reservedParser.errors().size() == 1 && reserved != nullptr &&
             reserved->phase == lang::DiagnosticPhase::Parsing &&
             reserved->primary.source == "reserved-identifier.gti" &&
             reserved->primary.start == reservedSource.find("template") &&
             reserved->primary.end == reservedSource.find("template") +
                                          std::string("template").size() &&
             reserved->primary.line == 1 &&
             reserved->message ==
                 "'template' is a reserved C++ keyword and cannot be used "
                 "as a GTI identifier." &&
             reserved->hints.empty() && reserved->fixes.empty(),
         "reserved C++ identifiers should produce one exact parser-owned "
         "diagnostic without an unsafe rename fix");
  expect(recovered.declarations().size() == 1,
         "a reserved identifier should not prevent recovery to a later "
         "declaration");

  const std::string deleteSource = "int delete = 1;\n"
                                   "int recovered = 2;\n";
  lang::Parser deleteParser(lexer.scan(deleteSource, "reserved-delete.gti"));
  const lang::Program deleteRecovered = deleteParser.parse();
  const lang::Diagnostic *deleteDiagnostic =
      findDiagnosticByCode(deleteParser.errors(), "GTI-P0002");
  expect(deleteParser.errors().size() == 1 && deleteDiagnostic != nullptr &&
             deleteDiagnostic->primary.start == deleteSource.find("delete") &&
             deleteDiagnostic->primary.end ==
                 deleteSource.find("delete") + std::string("delete").size() &&
             deleteDiagnostic->message ==
                 "'delete' is reserved for '= delete' special-member policy "
                 "and cannot be used as a GTI identifier." &&
             deleteDiagnostic->hints.empty() &&
             deleteDiagnostic->fixes.empty() &&
             deleteRecovered.declarations().size() == 1,
         "delete should be reserved outside its special-member policy");

  const std::vector<std::string> reservedSurfaces{
      "class template {};\n",
      "namespace template {}\n",
      "class Box { int template = 1; };\n",
      "int function(int template) { return 0; }\n",
      "class Values<uint64_t template> {};\n",
      "int main() { int values[template] = {}; return 0; }\n",
  };
  for (const std::string &source : reservedSurfaces) {
    lang::Parser surfaceParser(lexer.scan(source, "reserved-surface.gti"));
    surfaceParser.parse();
    const lang::Diagnostic *diagnostic =
        findDiagnosticByCode(surfaceParser.errors(), "GTI-P0002");
    expect(diagnostic != nullptr &&
               diagnostic->primary.start == source.find("template") &&
               diagnostic->primary.end ==
                   source.find("template") + std::string("template").size(),
           "reserved names should be rejected consistently across declaration "
           "and type-expression identifier surfaces");
  }

  const lang::FrontendResult issueReproduction = lang::Frontend().analyze(
      "issue-14.gti",
      "int main() { int template = 1; return template - 1; }\n");
  expect(
      !issueReproduction.syntaxValid && !issueReproduction.canGenerateCode() &&
          countDiagnosticCode(issueReproduction.diagnostics, "GTI-P0002") == 2,
      "issue #14 should diagnose each reserved occurrence in the GTI "
      "frontend before C++ emission");
}

void testDefaultImmutability() {
  lang::Lexer lexer;
  const std::string validSource = R"(
int identity(int value) { return value; }
int main() {
  int fixed = 1;
  mut int moving = 1;
  moving++;
  return identity(fixed);
}
)";
  auto validTokens = lexer.scan(validSource);
  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(), "mutability syntax should parse");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "mutable bindings should permit mutation");

  const std::string generated = emitAnalyzedSource(validSource);
  expect(generated.find("_identity(std::int32_t __gti_mir_arg_0)") !=
                 std::string::npos &&
             generated.find("__gti_mir_p_1 = __gti_mir_arg_0") !=
                 std::string::npos,
         "parameters should enter verified MIR through exact typed places");
  expect(generated.find("__gti_mir_p_1 = __gti_mir_v_1") != std::string::npos,
         "immutable variables should retain distinct MIR storage after "
         "semantic immutability checks");
  expect(
      generated.find("mir_checked_add_v1<std::int32_t>") != std::string::npos &&
          generated.find("__gti_mir_p_2 = __gti_mir_v_4") != std::string::npos,
      "mut variables should lower through explicit MIR updates");

  auto invalidTokens = lexer.scan(R"(
class Box {
  int value = 1;
  int change() { this.value = 2; return this.value; }
};
int changeParameter(int value) { value = 2; return value; }
int main() {
  int fixed = 1;
  fixed = 2;
  int missingInitializer;
  return 0;
}
)");
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(), "immutability error source should parse");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "immutable bindings should reject mutation");
  expect(invalidSemantic.errors().size() == 4,
         "members, parameters, locals, and missing initializers should fail");
}

void testDefaultNodiscard() {
  lang::Lexer lexer;
  const std::string validSource = R"(
int calculate() { return 7; }
void perform() {}
int main() {
  [[discard]] calculate();
  perform();
  mut int count = 0;
  count++;
  return 0;
}
)";
  auto validTokens = lexer.scan(validSource);
  expect(!lexer.hadError(), "discard attribute syntax should lex");

  lang::Parser validParser(std::move(validTokens));
  lang::Program validProgram = validParser.parse();
  expect(!validParser.hadError(), "discard attribute syntax should parse");

  lang::SemanticVisitor validSemantic;
  expect(validSemantic.check(validProgram),
         "explicit discard and void calls should pass semantic checks");

  const std::string generated = emitAnalyzedSource(validSource);
  expect(generated.find("calculate();") != std::string::npos &&
             generated.find("[[discard]]") == std::string::npos,
         "discard should be a GTI-only call-site attribute");

  const std::string invalidSource = R"(
int calculate() { return 7; }
namespace output {
int write(int value) { return value; }
}
void perform() {}
int main() {
  output::write(calculate());
  [[discard]] perform();
  [[discard]] 1 + 2;
  return 0;
}
)";
  auto invalidTokens = lexer.scan(invalidSource);
  lang::Parser invalidParser(std::move(invalidTokens));
  lang::Program invalidProgram = invalidParser.parse();
  expect(!invalidParser.hadError(),
         "invalid discard uses should remain semantic errors");

  lang::SemanticVisitor invalidSemantic;
  expect(!invalidSemantic.check(invalidProgram),
         "ignored function results and invalid discard attributes should fail");
  expect(invalidSemantic.errors().size() == 3,
         "nodiscard should produce focused diagnostics for all invalid uses");
  const lang::Diagnostic *ignored =
      findDiagnosticByCode(invalidSemantic.errors(), "GTI-S2009");
  expect(ignored != nullptr && ignored->group == "unused-result" &&
             ignored->fixes.size() == 1 &&
             ignored->fixes.front().span.start ==
                 invalidSource.find("output::write") &&
             ignored->fixes.front().span.end ==
                 invalidSource.find("output::write") &&
             ignored->fixes.front().replacement == "[[discard]] " &&
             !ignored->fixes.front().preferred,
         "an ignored nested result should insert an explicit, non-preferred "
         "discard fix at the complete expression start");
}

void testAdvisoryDiagnostics() {
  const lang::FrontendResult result =
      lang::Frontend().analyze("advisory-diagnostics.gti", R"(
int main() {
  mut int read_only = 1;
  mut int changed = 2;
  changed = 3;
  return read_only + changed;
  int unreachable = 4;
}
)");
  expect(result.canGenerateCode(),
         "warning-only programs should remain eligible for code generation");
  const lang::Diagnostic *unnecessary =
      findDiagnosticByCode(result.diagnostics, "GTI-S2096");
  expect(unnecessary != nullptr &&
             unnecessary->severity == lang::DiagnosticSeverity::Warning &&
             unnecessary->group == "unused-mut" &&
             unnecessary->tags ==
                 std::vector{lang::DiagnosticTag::Unnecessary} &&
             unnecessary->fixes.size() == 1 &&
             unnecessary->fixes.front().replacement.empty() &&
             unnecessary->fixes.front().preferred &&
             unnecessary->primary.start == std::string_view(R"(
int main() {
  mut int read_only = 1;
  mut int changed = 2;
  changed = 3;
  return read_only + changed;
  int unreachable = 4;
}
)")
                                               .find("mut int read_only"),
         "an unused local mut should have a stable warning group, unnecessary "
         "tag, and exact token removal fix");
  expect(countDiagnosticCode(result.diagnostics, "GTI-S2096") == 1,
         "a binding that is assigned later must retain mut without a warning");
  const lang::Diagnostic *unreachable =
      findDiagnosticByCode(result.diagnostics, "GTI-S2095");
  expect(unreachable != nullptr &&
             unreachable->severity == lang::DiagnosticSeverity::Warning &&
             unreachable->group == "unreachable-code" &&
             unreachable->tags ==
                 std::vector{lang::DiagnosticTag::Unnecessary} &&
             unreachable->fixes.empty(),
         "the first statement in an unreachable tail should be tagged but "
         "should not receive a semantics-changing deletion fix");

  const lang::FrontendResult requiredMutation =
      lang::Frontend().analyze("required-mutation.gti", R"(
void replace(mut int& value) { value = 3; }
struct Box {
  mut int value = 0;
  void change() mut { this.value = 4; }
};
int main() {
  mut int referenced = 1;
  replace(referenced);
  mut int addressed = 2;
  unsafe { int* pointer = &addressed; *pointer = 5; }
  mut Box box = Box();
  box.change();
  box.value += 1;
  mut int[1] values = {1};
  values[0] *= 2;
  return referenced + addressed + box.value + values[0];
}
)");
  expect(requiredMutation.canGenerateCode() &&
             countDiagnosticCode(requiredMutation.diagnostics, "GTI-S2096") ==
                 0,
         "mutable-reference arguments, mutable address-taking, and mutable "
         "receivers, field writes, and indexed writes should each preserve a "
         "required local mut qualifier");

  for (const std::string_view construction :
       {"mut View view{owner};", "mut View view = View(owner);",
        "mut View view = (View(owner));"}) {
    std::string source = R"(
struct Counter {
  mut int value;
  Counter(int initial) : value(initial) {}
};
class View {
  mut Counter& owner;
public:
  View(mut Counter& source) : owner(source) {}
  void advance() mut { this.owner.value += 1; }
};
int main() {
  mut Counter owner{4};
)";
    source += construction;
    source += "\n  view.advance();\n  return 0;\n}\n";
    const auto requiredConstructorMutation =
        lang::Frontend().analyze("mutable-constructor-advisory.gti", source);
    if (!requiredConstructorMutation.canGenerateCode() ||
        countDiagnosticCode(requiredConstructorMutation.diagnostics,
                            "GTI-S2096") != 0) {
      std::cerr << "Constructor advisory spelling: " << construction << '\n';
      for (const auto &diagnostic : requiredConstructorMutation.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    expect(requiredConstructorMutation.canGenerateCode() &&
               countDiagnosticCode(requiredConstructorMutation.diagnostics,
                                   "GTI-S2096") == 0,
           "a selected mutable-reference constructor parameter must preserve "
           "the argument owner's required mut qualifier for every construction "
           "spelling");

    source.erase(source.find("mut Counter owner"), 4);
    const auto missingConstructorMutation =
        lang::Frontend().analyze("immutable-constructor-advisory.gti", source);
    expect(!missingConstructorMutation.canGenerateCode() &&
               findDiagnosticByCode(missingConstructorMutation.diagnostics,
                                    "GTI-S2012") != nullptr,
           "removing the mutable constructor argument's mut must remain "
           "invalid rather than becoming an advisory fix");
  }

  const std::string readOnlyConstructorSource = R"(
class Inspect {
public:
  Inspect(int& source) {}
};
int main() {
  mut int owner = 4;
  Inspect view{owner};
  return owner;
}
)";
  const auto readOnlyConstructor = lang::Frontend().analyze(
      "readonly-constructor-advisory.gti", readOnlyConstructorSource);
  const auto *readOnlyWarning =
      findDiagnosticByCode(readOnlyConstructor.diagnostics, "GTI-S2096");
  expect(readOnlyConstructor.canGenerateCode() && readOnlyWarning != nullptr &&
             countDiagnosticCode(readOnlyConstructor.diagnostics,
                                 "GTI-S2096") == 1 &&
             readOnlyWarning->primary.start ==
                 readOnlyConstructorSource.find("mut int owner") &&
             readOnlyWarning->fixes.size() == 1 &&
             readOnlyWarning->fixes.front().replacement.empty() &&
             readOnlyWarning->fixes.front().preferred,
         "a read-only constructor argument must retain the safe unnecessary "
         "mut warning and exact token-removal fix");

  const auto payloadBorrow =
      lang::Frontend().analyze("mutable-payload-advisory.gti", R"(
enum class Data { some(int item), none };
int main() {
  mut Data input = Data::some(3);
  switch (input) {
    case Data::some(mut auto& item): item = 7; return item;
    case Data::none: return 0;
  }
}
)");
  expect(payloadBorrow.canGenerateCode() &&
             countDiagnosticCode(payloadBorrow.diagnostics, "GTI-S2096") == 0,
         "a mutable payload borrow requires its enum owner's mut qualifier "
         "and must not offer an invalid removal fix");
}

void testPrintIsAnIdentifier() {
  lang::Lexer lexer;
  const std::string source = R"(
int print(int value) { return value; }
int main() { return print(0); }
)";
  auto tokens = lexer.scan(source);
  lang::Parser parser(std::move(tokens));
  lang::Program program = parser.parse();
  expect(!parser.hadError(), "print should parse as an ordinary identifier");

  lang::SemanticVisitor semantic;
  expect(semantic.check(program),
         "user code should be able to declare a function named print");

  const std::string generated = emitAnalyzedSource(source);
  expect(generated.find("std::int32_t __gti_fn_") != std::string::npos &&
             generated.find("_print(std::int32_t __gti_mir_arg_0)") !=
                 std::string::npos &&
             generated.find("_print(__gti_mir_v_") != std::string::npos,
         "print should lower as a normal function");
}

void testFormatting() {
  const std::string source = R"(#include   "math.gti"
#include   < std / array >

namespace engine{class Counter{mut int value=0;
#if target.arch=="arm64"
int word_bits=64;
#endif
int tick(int amount)mut{if(amount>0){this.value+=amount;}else{this.value-=1;}return this.value;}};}
#if target.vendor=="apple"
int main(){for(mut int i=0;i<3;i++){if(i==1){continue ;}std::println("frame"); // keep this comment
if(i>1){break ;}
}mut int attempts=0;do{attempts++;}while(attempts<1);return -1;}
#else
int main(){[[discard]] engine::run();return 0;}
#endif
)";

  const std::string expected = R"(#include "math.gti"
#include <std/array>

namespace engine {
  class Counter {
    mut int value = 0;
#if target.arch == "arm64"
    int word_bits = 64;
#endif
    int tick(int amount) mut {
      if (amount > 0) {
        this.value += amount;
      } else {
        this.value -= 1;
      }
      return this.value;
    }
  };
}
#if target.vendor == "apple"
int main() {
  for (mut int i = 0; i < 3; i++) {
    if (i == 1) {
      continue;
    }
    std::println("frame"); // keep this comment
    if (i > 1) {
      break;
    }
  }
  mut int attempts = 0;
  do {
    attempts++;
  } while (attempts < 1);
  return -1;
}
#else
int main() {
  [[discard]] engine::run();
  return 0;
}
#endif
)";

  const std::string formatted = lang::Formatter().format(source);
  if (formatted != expected) {
    std::cerr << "Formatted output was:\n" << formatted;
  }
  expect(formatted == expected,
         "formatter should produce stable C++-style GTI layout");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatting should be idempotent");

  const std::string nestedLambda = lang::Formatter().format(
      "void invoke<Callback>(Callback callback){callback();}"
      "void run(){invoke([]()->void{mut auto result=login();if(!result){"
      "report();}for(mut int i=0;i<3;i++){tick();}});}");
  expect(nestedLambda.find("login();\n    if (!result)") != std::string::npos &&
             nestedLambda.find("for (mut int i = 0; i < 3; i++)") !=
                 std::string::npos &&
             lang::Formatter().format(nestedLambda) == nestedLambda,
         "statements in a lambda argument should break at the lambda block "
         "depth without splitting for-loop header semicolons");

  const auto initializerLayout = [](const std::string &input,
                                    const std::string &expected) {
    const std::string output = lang::Formatter().format(input);
    if (output != expected)
      std::cerr << "Initializer formatting was:\n" << output;
    expect(output == expected, "initializer layout should retain nesting");
    expect(lang::Formatter().format(output) == output,
           "initializer layout should be idempotent");
    lang::Lexer beforeLexer;
    lang::Lexer afterLexer;
    const auto before = beforeLexer.scan(input);
    const auto after = afterLexer.scan(output);
    expect(!before.empty() && before.size() == after.size() &&
               std::equal(before.begin(), std::prev(before.end()),
                          after.begin(),
                          [](const auto &a, const auto &b) {
                            return a.kind == b.kind && a.lexeme == b.lexeme;
                          }),
           "initializer formatting must preserve tokens, even on broken input");
  };
  const std::string schemaSource =
      "void setup(){Command schema={.name=std::string{\"tea\"},"
      ".subcommands={{.name=std::string{\"auth\"},"
      ".subcommands={{.name=std::string{\"login\"}}}}}};}";
  initializerLayout(schemaSource, R"(void setup() {
  Command schema = {
    .name = std::string{"tea"},
    .subcommands = {
      {
        .name = std::string{"auth"},
        .subcommands = {
          {
            .name = std::string{"login"}
          }
        }
      }
    }
  };
}
)");
  initializerLayout("C c={.a=call(1,2),.b=make<int,int>(3,4),"
                    ".c=[left,right](int a,int b){return a+b;},.d={}};",
                    R"(C c = {
  .a = call(1, 2),
  .b = make<int, int>(3, 4),
  .c = [left, right](int a, int b) {
    return a + b;
  },
  .d = {}
};
)");
  initializerLayout("int values[3]={\n1,2,3,\n};make({1,2},{3,4});",
                    "int values[3] = {\n  1,\n  2,\n  3,\n};\n"
                    "make({1, 2}, {3, 4});\n");
  initializerLayout("C c={ // header\n.name=\"{,} 😀\", // name\n"
                    ".count=call(1,2), /* count */ .empty={\n}};",
                    "C c = { // header\n  .name = \"{,} 😀\", // name\n"
                    "  .count = call(1, 2), /* count */\n  .empty = {}\n};\n");
  initializerLayout("C root={.name=\"tea\",.children={.name=\"auth\"}}",
                    "C root = {\n  .name = \"tea\",\n  .children = {\n"
                    "    .name = \"auth\"\n  }\n}\n");
  initializerLayout("C root={.name=\"tea\",.children={\n{.name=",
                    "C root = {\n  .name = \"tea\",\n  .children = {\n"
                    "    {\n      .name =\n");
  const auto tabs =
      lang::Formatter({.indentWidth = 4,
                       .insertSpaces = false,
                       .breakBeforeBraces = lang::BraceBreakingStyle::Allman})
          .format("void f(){C c={.n=1,.child={.n=2}};}");
  expect(tabs == "void f()\n{\n\tC c = {\n\t\t.n = 1,\n"
                 "\t\t.child = {\n\t\t\t.n = 2\n\t\t}\n\t};\n}\n" &&
             lang::Formatter(
                 {.indentWidth = 4,
                  .insertSpaces = false,
                  .breakBeforeBraces = lang::BraceBreakingStyle::Allman})
                     .format(tabs) == tabs,
         "initializer indentation honors tabs without Allman block wrapping");

  const std::string separatedAmpersands = lang::Formatter().format("& &m");
  expect(separatedAmpersands == "& &m\n" &&
             lang::Formatter().format(separatedAmpersands) ==
                 separatedAmpersands,
         "formatting must preserve a source token boundary between adjacent "
         "ampersands");
  for (const std::string_view boundary :
       {"+ +value", "- -value", "/ *p", "/ /p", ". . .", "1 . 2"}) {
    const std::string boundaryFormatted = lang::Formatter().format(boundary);
    lang::Lexer sourceLexer;
    lang::Lexer formattedLexer;
    const std::vector<lang::Token> sourceTokens = sourceLexer.scan(
        std::string(boundary), "formatter-boundary-source.gti");
    const std::vector<lang::Token> formattedTokens = formattedLexer.scan(
        boundaryFormatted, "formatter-boundary-formatted.gti");
    const bool tokenEquivalent =
        sourceTokens.size() == formattedTokens.size() &&
        !sourceTokens.empty() &&
        sourceTokens.back().kind == lang::TokenKind::END_OF_FILE &&
        formattedTokens.back().kind == lang::TokenKind::END_OF_FILE &&
        std::equal(sourceTokens.begin(), std::prev(sourceTokens.end()),
                   formattedTokens.begin(),
                   [](const lang::Token &source, const lang::Token &formatted) {
                     return source.kind == formatted.kind &&
                            source.lexeme == formatted.lexeme;
                   });
    expect(tokenEquivalent &&
               lang::Formatter().format(boundaryFormatted) == boundaryFormatted,
           "maximal-munch token boundaries should remain formatter-"
           "equivalent and idempotent for '" +
               std::string(boundary) + "'");
  }

  const std::string staticFormatted = lang::Formatter().format(
      "static mut int count=0;class Registry{public:static int value=1;"
      "static int current(){return value;}};");
  expect(staticFormatted == R"(static mut int count = 0;
class Registry {
public:
  static int value = 1;
  static int current() {
    return value;
  }
};
)" && lang::Formatter().format(staticFormatted) == staticFormatted,
         "formatter should preserve static declaration ownership and remain "
         "idempotent");

  const std::string constexprFormatted = lang::Formatter().format(
      "constexpr uint64_t extent=uint64_t(2+2);class Limits{public:static "
      "constexpr uint64_t count=extent;};int choice(){if constexpr(true){"
      "return 1;}else{return 0;}}");
  expect(constexprFormatted == R"(constexpr uint64_t extent = uint64_t(2 + 2);
class Limits {
public:
  static constexpr uint64_t count = extent;
};
int choice() {
  if constexpr (true) {
    return 1;
  } else {
    return 0;
  }
}
)" && lang::Formatter().format(constexprFormatted) == constexprFormatted,
         "formatter should preserve constexpr declarations and branches and "
         "remain idempotent");

  const std::string tabIndented =
      lang::Formatter({.indentWidth = 4, .insertSpaces = false})
          .format("int main(){if(true){return 0;}}");
  expect(tabIndented.find("\n\tif (true) {\n\t\treturn 0;") !=
             std::string::npos,
         "formatter should honor tab indentation requested by an editor");

  const std::string referenceSource =
      "class Box<T>{};"
      "void use(int& value,Box<int>& box){int A=value;int bits=A&value;}";
  const std::string leftReferences =
      lang::Formatter({.referenceAlignment = lang::ReferenceAlignment::Left})
          .format(referenceSource);
  const std::string rightReferences =
      lang::Formatter({.referenceAlignment = lang::ReferenceAlignment::Right})
          .format(referenceSource);
  const std::string defaultReferences = lang::Formatter().format(
      "class Box{};void use(int& value,Box& box){int bits=value&value;}"
      "int min(int left,int right){if(left> right){return right;}return "
      "left;}int invoke(){return 1;}int expression(){mut int A=1;"
      "A&invoke();return A;}");
  const std::string middleReferences =
      lang::Formatter({.referenceAlignment = lang::ReferenceAlignment::Middle})
          .format(referenceSource);
  expect(leftReferences.find("int& value") != std::string::npos &&
             leftReferences.find("Box<int>& box") != std::string::npos &&
             rightReferences.find("int &value") != std::string::npos &&
             rightReferences.find("Box<int> &box") != std::string::npos &&
             leftReferences.find("A & value") != std::string::npos &&
             rightReferences.find("A & value") != std::string::npos &&
             defaultReferences.find("int& value") != std::string::npos &&
             defaultReferences.find("Box& box") != std::string::npos &&
             defaultReferences.find("A & invoke();") != std::string::npos &&
             middleReferences.find("int & value") != std::string::npos,
         "reference alignment should default left, preserve explicit styles, "
         "and recognize declared generic types without changing bitwise-and "
         "spacing for capitalized values or call expressions");
  expect(defaultReferences.find("if (left > right)") != std::string::npos,
         "comparison operators should retain binary spacing");

  const std::string importedReturnReferences = lang::Formatter().format(
      "class Application{public:mut LayerStack & GetLayerStack()mut{"
      "return this.m_LayerStack;}LayerStack & PeekLayerStack(){return "
      "this.m_LayerStack;}static mut Application & GetApplication(){unsafe{"
      "return *Application::app;}}};");
  expect(importedReturnReferences.find(
             "mut LayerStack& GetLayerStack() mut {") != std::string::npos &&
             importedReturnReferences.find("LayerStack& PeekLayerStack() {") !=
                 std::string::npos &&
             importedReturnReferences.find(
                 "static mut Application& GetApplication() {") !=
                 std::string::npos &&
             lang::Formatter().format(importedReturnReferences) ==
                 importedReturnReferences,
         "default left alignment should cover included nominal return types "
         "and remain idempotent");

  const std::string dependentDeclarators = lang::Formatter().format(
      "T & borrow<T>(T & value){return value;}"
      "T * address<T>(T * value){return value;}"
      "virtual void observe(External & reference,External * pointer);"
      "int expressions(int left,int right){mut External * handle=nullptr;"
      "int product=left*right;return left&right;}");
  const std::string expectedDependentDeclarators = R"(T& borrow<T>(T& value) {
  return value;
}
T* address<T>(T* value) {
  return value;
}
virtual void observe(External& reference, External* pointer);
int expressions(int left, int right) {
  mut External* handle = nullptr;
  int product = left * right;
  return left & right;
}
)";
  if (dependentDeclarators != expectedDependentDeclarators) {
    std::cerr << "Dependent declarator formatting was:\n"
              << dependentDeclarators;
  }
  expect(dependentDeclarators == expectedDependentDeclarators &&
             lang::Formatter().format(dependentDeclarators) ==
                 dependentDeclarators,
         "default declarator alignment should attach generic and imported "
         "pointer/reference markers to their types without changing binary "
         "operator spacing");

  const std::string pointerFormatted = lang::Formatter().format(
      "class Byte{};void use(mut uint8_t * data,const uint8_t*read_only){"
      "mut uint8_t*alias=& data[0];* data=read_only [0];"
      "uint8_t product=data[0]*read_only[0];"
      "uint8_t bits=data[0]&read_only[0];return * alias;}"
      "class Link{public:uint8_t value=0;};"
      "uint8_t member(Link*link){return link -> value;}"
      "class RawBox<T>{T * value;const T * read(){return value;}};");
  const std::string expectedPointerFormatting = R"(class Byte {};
void use(mut uint8_t* data, const uint8_t* read_only) {
  mut uint8_t* alias = &data[0];
  *data = read_only[0];
  uint8_t product = data[0] * read_only[0];
  uint8_t bits = data[0] & read_only[0];
  return *alias;
}
class Link {
public:
  uint8_t value = 0;
};
uint8_t member(Link* link) {
  return link->value;
}
class RawBox<T> {
  T* value;
  const T* read() {
    return value;
  }
};
)";
  expect(pointerFormatted == expectedPointerFormatting &&
             lang::Formatter().format(pointerFormatted) == pointerFormatted,
         "formatter should distinguish pointer declarators and unary raw "
         "pointer operators from multiplication and bitwise-and");

  const std::string commentedGenericReturn = lang::Formatter().format(
      "namespace detail{class Iterator<T>{};}class Range<T>{public:"
      "// Iterator access remains read-only.\n"
      "detail::Iterator<T> begin();detail::Iterator<Range<T>> nested();"
      "bool ordered(int left,int right){return left < right;}};");
  expect(commentedGenericReturn.find("// Iterator access remains read-only.\n"
                                     "  detail::Iterator<T> begin();") !=
                 std::string::npos &&
             commentedGenericReturn.find(
                 "detail::Iterator<Range<T>> nested();") != std::string::npos,
         "comments should not make generic return types format as relational "
         "expressions");
  expect(commentedGenericReturn.find("return left < right;") !=
                 std::string::npos &&
             lang::Formatter().format(commentedGenericReturn) ==
                 commentedGenericReturn,
         "generic return formatting should preserve comparisons and remain "
         "idempotent");

  const lang::FormatConfigResult parsedStyle =
      lang::parseFormatConfig("IndentWidth: 4\n"
                              "BasedOnStyle: LLVM\n"
                              "UseTab: Never\n"
                              "BreakBeforeBraces: Allman\n"
                              "SpaceBeforeParens: Never\n"
                              "IndentCaseLabels: true\n"
                              "AccessModifierOffset: -4\n"
                              "MaxEmptyLinesToKeep: 0\n"
                              "SpacesBeforeTrailingComments: 3\n"
                              "SpaceBeforeAssignmentOperators: false\n"
                              "ReferenceAlignment: Left\n",
                              {.indentWidth = 7});
  expect(parsedStyle.issues.empty() && parsedStyle.options.indentWidth == 4 &&
             parsedStyle.options.insertSpaces &&
             parsedStyle.options.breakBeforeBraces ==
                 lang::BraceBreakingStyle::Allman &&
             parsedStyle.options.spaceBeforeParens ==
                 lang::SpaceBeforeParensStyle::Never &&
             parsedStyle.options.indentCaseLabels &&
             parsedStyle.options.accessModifierOffset == -4 &&
             parsedStyle.options.maxEmptyLinesToKeep == 0 &&
             parsedStyle.options.spacesBeforeTrailingComments == 3 &&
             !parsedStyle.options.spaceBeforeAssignmentOperators &&
             parsedStyle.options.referenceAlignment ==
                 lang::ReferenceAlignment::Left,
         "format configuration should apply a base style before explicit "
         "clang-format-compatible overrides");

  const lang::FormatConfigResult defaultStyle =
      lang::parseFormatConfig(lang::defaultFormatConfig());
  expect(defaultStyle.issues.empty() &&
             defaultStyle.options.referenceAlignment ==
                 lang::ReferenceAlignment::Left,
         "the generated default format configuration should parse back to "
         "the GTI default style");

  const std::string configuredSource =
      "class Example{public:int run(int value){if(value>0){value+=1;"
      "// retain\n}else{return 0;}switch(value){case 1:value+=1;break;"
      "default:return value;}}};";
  const std::string configuredExpected = R"(class Example
{
public:
    int run(int value)
    {
        if(value > 0)
        {
            value+= 1;   // retain
        }
        else
        {
            return 0;
        }
        switch(value)
        {
            case 1:
                value+= 1;
                break;
            default:
                return value;
        }
    }
};
)";
  const std::string configured =
      lang::Formatter(parsedStyle.options).format(configuredSource);
  if (configured != configuredExpected) {
    std::cerr << "Configured formatted output was:\n" << configured;
  }
  expect(configured == configuredExpected &&
             lang::Formatter(parsedStyle.options).format(configured) ==
                 configured,
         "configured Allman, spacing, access, case-label, and comment styles "
         "should be stable and idempotent");

  const lang::FormatConfigResult tabStyle =
      lang::parseFormatConfig("IndentWidth: 4\nUseTab: ForIndentation\n");
  expect(!tabStyle.options.insertSpaces &&
             lang::Formatter(tabStyle.options)
                     .format("int main(){if(true){return 0;}}")
                     .find("\n\tif (true) {") != std::string::npos,
         "UseTab: ForIndentation should use tabs for structural indentation");

  const std::string parenthesizedReturn = lang::Formatter().format(
      "T midpoint<std::numeric T>(T left,T right){return(left+right)/2;}");
  const std::string expectedParenthesizedReturn =
      "T midpoint<std::numeric T>(T left, T right) {\n"
      "  return (left + right) / 2;\n"
      "}\n";
  expect(parenthesizedReturn == expectedParenthesizedReturn &&
             lang::Formatter().format(parenthesizedReturn) ==
                 parenthesizedReturn,
         "default formatting should separate return from a parenthesized "
         "expression idempotently");

  const std::string parenthesizedControlSource =
      "int select(bool ready){if(ready){return(1);}return(0);}";
  const lang::Formatter neverParentheses(
      {.spaceBeforeParens = lang::SpaceBeforeParensStyle::Never});
  const std::string neverParenthesizedReturn =
      neverParentheses.format(parenthesizedControlSource);
  expect(
      neverParenthesizedReturn.find("if(ready)") != std::string::npos &&
          neverParenthesizedReturn.find("return (1);") != std::string::npos &&
          neverParenthesizedReturn.find("return (0);") != std::string::npos &&
          neverParentheses.format(neverParenthesizedReturn) ==
              neverParenthesizedReturn,
      "SpaceBeforeParens: Never should not remove the separator after "
      "return");

  const std::string spacedParentheses =
      lang::Formatter(
          {.spaceBeforeParens = lang::SpaceBeforeParensStyle::Always})
          .format("int invoke<T>(T value){if(true){return invoke<T>(value);}}");
  expect(spacedParentheses.find("int invoke<T> (T value)") !=
                 std::string::npos &&
             spacedParentheses.find("if (true)") != std::string::npos &&
             spacedParentheses.find("invoke<T> (value)") != std::string::npos,
         "SpaceBeforeParens: Always should cover declarations, controls, and "
         "generic calls");
  const lang::Formatter alwaysParentheses(
      {.spaceBeforeParens = lang::SpaceBeforeParensStyle::Always});
  const std::string alwaysParenthesizedReturn =
      alwaysParentheses.format(parenthesizedControlSource);
  expect(
      alwaysParenthesizedReturn.find("int select (bool ready)") !=
              std::string::npos &&
          alwaysParenthesizedReturn.find("if (ready)") != std::string::npos &&
          alwaysParenthesizedReturn.find("return (1);") != std::string::npos &&
          alwaysParentheses.format(alwaysParenthesizedReturn) ==
              alwaysParenthesizedReturn,
      "SpaceBeforeParens: Always should retain the separator after return "
      "idempotently");

  const lang::FormatConfigResult invalidStyle =
      lang::parseFormatConfig("IndentWidth: 0\n"
                              "UseTab: Sometimes\n"
                              "UnknownOption: value\n"
                              "not-an-entry\n");
  expect(invalidStyle.issues.size() == 4,
         "invalid and unsupported format entries should remain observable");

  const std::string emptyLines = lang::Formatter({.maxEmptyLinesToKeep = 2})
                                     .format("int first=1;\n\n\nint second=2;");
  expect(emptyLines == "int first = 1;\n\n\nint second = 2;\n" &&
             lang::Formatter({.maxEmptyLinesToKeep = 2}).format(emptyLines) ==
                 emptyLines,
         "MaxEmptyLinesToKeep should cap and preserve the requested number "
         "of empty lines");

  const std::string disabledSource = "int main(){return 0;}";
  expect(lang::Formatter({.disableFormat = true}).format(disabledSource) ==
             disabledSource,
         "DisableFormat should leave source byte-for-byte unchanged");
}

void testLanguageQueries() {
  const std::string source = R"(
/// Chooses the unsigned value.
///
/// The exact overload is selected by semantics.
uint64_t choose(uint64_t value) { return value; }
float choose(float value) { return value; }

namespace engine {
namespace graphics {
int render() { return 1; }
}
}
/// A shorter spelling for the graphics namespace.
namespace gfx = engine::graphics;

class Fixed<T, uint64_t N> {
  T values[N] = {};
};

/// Owns one integer value.
class Box {
  int value = 0;
public:
  /// Constructs a box from `value`.
  Box(int value) : value(value) {}
};

class Lifecycle {
  int value = 0;
public:
  Lifecycle(Lifecycle& other) = default;
  Lifecycle(Lifecycle&& other) = default;
};

class Accessor {
  /// The value returned by `inspect`.
  mut int value = 0;
public:
  int inspect() { return this.value; }
  int inspect() mut {
    this.value += 1;
    return this.value;
  }
};

/// A type that can render itself.
interface Renderable {
  int render();
};

void relay<Args...>(Args... values) {}

/// This block is deliberately detached.

class Undocumented {
  int value = 0;
};

// Ordinary comments are not API documentation.
class OrdinaryCommented {
  int value = 0;
};

int main() {
  auto inferred = choose(uint64_t(1));
  int rendered = gfx::render();
  mut int counter = 0;
  counter++;
  ++counter;
  Box box = Box(1);
  Lifecycle lifecycle_source{};
  Lifecycle lifecycle_copy = Lifecycle(lifecycle_source);
  mut Lifecycle lifecycle_movable{};
  Lifecycle lifecycle_move = Lifecycle(std::move(lifecycle_movable));
  Accessor fixed = Accessor();
  mut Accessor changing = Accessor();
  int fixed_value = fixed.inspect();
  int changing_value = changing.inspect();
  return 0;
}
)";
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "language-queries.gti", source, {standardLibraryPrelude()});
  expect(frontend.semanticValid,
         "language-query fixture should pass semantic analysis");

  const lang::SourceUnitId unit = frontend.sourceGraph.entryUnit();
  const lang::LanguageQueries queries;
  const auto hoverAt = [&](std::size_t offset) {
    return queries.hover(frontend, unit, offset);
  };

  const std::size_t call = source.rfind("choose(uint64_t");
  const std::optional<lang::HoverInfo> selected = hoverAt(call + 1);
  expect(selected && selected->signature == "uint64_t choose(uint64_t value)",
         "hover should render the exact selected overload in GTI syntax");
  expect(selected && selected->documentationMarkdown ==
                         "Chooses the unsigned value.\n\nThe exact overload "
                         "is selected by semantics.",
         "selected-call hover should carry normalized declaration Markdown");
  expect(selected && selected->signature.find("float") == std::string::npos,
         "selected-call hover should not include unrelated overloads");
  expect(selected && selected->range.start == call &&
             selected->range.end == call + std::string("choose").size(),
         "hover should retain the exact source identifier range");

  const std::size_t interfaceDeclaration = source.find("Renderable");
  const std::optional<lang::HoverInfo> interfaceHover =
      hoverAt(interfaceDeclaration + 1);
  expect(interfaceHover && interfaceHover->signature == "interface Renderable",
         "hover should preserve interface spelling instead of rendering a "
         "class");
  expect(interfaceHover && interfaceHover->documentationMarkdown ==
                               "A type that can render itself.",
         "type hover should carry declaration documentation");

  const std::size_t variadicDeclaration = source.find("relay<Args");
  const std::optional<lang::HoverInfo> variadicHover =
      hoverAt(variadicDeclaration + 1);
  expect(variadicHover &&
             variadicHover->signature == "void relay<Args...>(Args... values)",
         "hover should preserve variadic-pack spelling on declarations");

  const std::size_t undocumentedDeclaration = source.find("Undocumented");
  const std::size_t ordinaryCommentedDeclaration =
      source.find("OrdinaryCommented");
  const std::optional<lang::HoverInfo> undocumentedHover =
      hoverAt(undocumentedDeclaration + 1);
  const std::optional<lang::HoverInfo> ordinaryCommentedHover =
      hoverAt(ordinaryCommentedDeclaration + 1);
  expect(undocumentedHover && !undocumentedHover->documentationMarkdown &&
             ordinaryCommentedHover &&
             !ordinaryCommentedHover->documentationMarkdown,
         "blank lines and ordinary comments should not attach API Markdown");

  const std::size_t readonlyReceiverCall =
      source.find("fixed.inspect") + std::string("fixed.").size();
  const std::size_t mutableReceiverCall =
      source.find("changing.inspect") + std::string("changing.").size();
  const std::optional<lang::HoverInfo> readonlyReceiverHover =
      hoverAt(readonlyReceiverCall + 1);
  const std::optional<lang::HoverInfo> mutableReceiverHover =
      hoverAt(mutableReceiverCall + 1);
  expect(
      readonlyReceiverHover &&
          readonlyReceiverHover->signature == "int32_t Accessor::inspect()" &&
          mutableReceiverHover &&
          mutableReceiverHover->signature == "int32_t Accessor::inspect() mut",
      "hover should expose the exact receiver-qualified method selected by "
      "semantics");

  const std::size_t autoKeyword = source.find("auto inferred");
  const std::optional<lang::HoverInfo> inferredType = hoverAt(autoKeyword + 1);
  expect(inferredType && inferredType->signature == "uint64_t",
         "hover over auto should show the complete inferred type");

  const std::size_t inferredName = source.find("inferred", autoKeyword);
  const std::optional<lang::HoverInfo> binding = hoverAt(inferredName + 1);
  expect(binding && binding->signature == "uint64_t inferred",
         "hover over a binding declaration should show mutability and type");

  const std::size_t construction = source.rfind("Box(1)");
  const std::optional<lang::HoverInfo> constructor = hoverAt(construction + 1);
  expect(constructor && constructor->signature == "Box(int32_t value)",
         "constructor hover should show the selected constructor signature");
  expect(constructor && constructor->documentationMarkdown ==
                            "Constructs a box from `value`.",
         "constructor-call hover should prefer constructor documentation");

  const std::size_t boxTypeUse = source.find("Box box");
  const std::optional<lang::HoverInfo> boxTypeHover = hoverAt(boxTypeUse + 1);
  expect(boxTypeHover &&
             boxTypeHover->documentationMarkdown == "Owns one integer value.",
         "type-use hover should carry documentation from its semantic symbol");

  const std::size_t fieldUse =
      source.find("this.value") + std::string("this.").size();
  const std::optional<lang::HoverInfo> fieldHover = hoverAt(fieldUse + 1);
  expect(fieldHover && fieldHover->documentationMarkdown ==
                           "The value returned by `inspect`.",
         "field-use hover should carry field documentation");

  const std::size_t copyConstruction =
      source.rfind("Lifecycle(lifecycle_source)");
  const std::size_t moveConstruction =
      source.rfind("Lifecycle(std::move(lifecycle_movable))");
  const std::optional<lang::HoverInfo> copyConstructor =
      hoverAt(copyConstruction + 1);
  const std::optional<lang::HoverInfo> moveConstructor =
      hoverAt(moveConstruction + 1);
  expect(copyConstructor &&
             copyConstructor->signature == "Lifecycle(Lifecycle&)" &&
             moveConstructor &&
             moveConstructor->signature == "Lifecycle(Lifecycle&&)",
         "constructor hover should distinguish copy and move policies");

  expect(!hoverAt(source.find("return 0") + std::string("return").size()),
         "hover should return no result for whitespace");
  expect(!frontend.semantics.database().occurrences(unit).empty(),
         "semantic analysis should retain tooling occurrences in its snapshot");

  const auto parameterReference = std::find_if(
      frontend.semantics.database().occurrences(unit).begin(),
      frontend.semantics.database().occurrences(unit).end(),
      [](const lang::SemanticOccurrence &occurrence) {
        return occurrence.name == "value" &&
               !lang::hasRole(occurrence.roles,
                              lang::OccurrenceRole::Declaration) &&
               occurrence.bindingKind == lang::SemanticBindingKind::Parameter;
      });
  expect(parameterReference !=
             frontend.semantics.database().occurrences(unit).end(),
         "semantic occurrences should retain resolved parameter references");
  if (parameterReference !=
      frontend.semantics.database().occurrences(unit).end()) {
    const lang::SymbolRecord *parameterSymbol =
        frontend.semantics.database().findSymbol(parameterReference->symbol);
    expect(parameterSymbol != nullptr &&
               parameterSymbol->kind == lang::SymbolKind::Parameter &&
               parameterSymbol->name == "value",
           "resolved parameter uses should point to a compiler-owned symbol");
    expect(frontend.semantics.database()
                   .occurrencesForSymbol(parameterReference->symbol)
                   .size() >= 2,
           "symbol queries should connect a parameter declaration and use");
  }

  const auto counterMutation = std::find_if(
      frontend.semantics.database().occurrences(unit).begin(),
      frontend.semantics.database().occurrences(unit).end(),
      [](const lang::SemanticOccurrence &occurrence) {
        return occurrence.name == "counter" &&
               lang::hasRole(occurrence.roles, lang::OccurrenceRole::Read) &&
               lang::hasRole(occurrence.roles, lang::OccurrenceRole::Write);
      });
  expect(counterMutation !=
             frontend.semantics.database().occurrences(unit).end(),
         "increment operands should retain combined read/write roles");

  const std::optional<lang::DefinitionInfo> selectedDefinition =
      queries.definition(frontend, unit, call + 1);
  const std::size_t firstChoose = source.find("choose(uint64_t");
  expect(selectedDefinition &&
             selectedDefinition->target.start == firstChoose &&
             selectedDefinition->target.end ==
                 firstChoose + std::string("choose").size(),
         "definition should follow the exact selected overload");

  const std::optional<lang::DefinitionInfo> readonlyReceiverDefinition =
      queries.definition(frontend, unit, readonlyReceiverCall + 1);
  const std::optional<lang::DefinitionInfo> mutableReceiverDefinition =
      queries.definition(frontend, unit, mutableReceiverCall + 1);
  const std::size_t readonlyReceiverDeclaration = source.find("inspect()");
  const std::size_t mutableReceiverDeclaration =
      source.find("inspect() mut", readonlyReceiverDeclaration + 1);
  expect(readonlyReceiverDefinition && mutableReceiverDefinition &&
             readonlyReceiverDefinition->target.start ==
                 readonlyReceiverDeclaration &&
             mutableReceiverDefinition->target.start ==
                 mutableReceiverDeclaration,
         "definition should follow receiver-qualified method identities");

  const std::size_t aliasUse = source.find("gfx::render");
  const std::optional<lang::DefinitionInfo> aliasDefinition =
      queries.definition(frontend, unit, aliasUse + 1);
  const std::size_t aliasDeclaration =
      source.find("gfx", source.find("namespace gfx"));
  expect(aliasDefinition && aliasDefinition->target.start == aliasDeclaration,
         "definition should preserve namespace alias identity");

  const std::size_t aliasTarget =
      source.find("engine::graphics", aliasDeclaration);
  const std::optional<lang::DefinitionInfo> namespaceDefinition =
      queries.definition(frontend, unit, aliasTarget + 1);
  const std::size_t namespaceDeclaration =
      source.find("engine", source.find("namespace engine"));
  expect(namespaceDefinition &&
             namespaceDefinition->target.start == namespaceDeclaration,
         "definition on an alias target should resolve its namespace");

  const std::size_t valueParameterUse = source.find("[N]") + 1;
  const std::optional<lang::DefinitionInfo> valueParameterDefinition =
      queries.definition(frontend, unit, valueParameterUse);
  const std::size_t valueParameterDeclaration =
      source.find("N>", source.find("class Fixed"));
  expect(valueParameterDefinition && valueParameterDefinition->target.start ==
                                         valueParameterDeclaration,
         "definition should connect value-generic uses to their parameter");

  const std::optional<lang::DefinitionInfo> classDefinition =
      queries.definition(frontend, unit, boxTypeUse + 1);
  const std::size_t boxClass = source.find("Box", source.find("class Box"));
  expect(classDefinition && classDefinition->target.start == boxClass,
         "definition on a declared type should target the class declaration");

  const std::optional<lang::DefinitionInfo> constructorDefinition =
      queries.definition(frontend, unit, construction + 1);
  const std::size_t boxConstructor = source.find("Box(int value)");
  expect(constructorDefinition &&
             constructorDefinition->target.start == boxConstructor,
         "definition on a construction call should target its constructor");
  expect(!queries.definition(frontend, unit,
                             source.find("return 0") +
                                 std::string("return").size()),
         "definition should fail closed when no resolved symbol is present");

  const auto complete = [&](const std::string &completionSource,
                            std::string_view prefix) {
    const std::size_t offset = completionSource.rfind(prefix);
    expect(offset != std::string::npos,
           "completion test fixture should contain its cursor prefix");
    return queries.complete({.entryPath = "completion.gti",
                             .source = completionSource,
                             .byteOffset = offset + prefix.size()});
  };
  const auto findCandidate = [](const lang::CompletionResult &completion,
                                std::string_view label) {
    return std::find_if(completion.candidates.begin(),
                        completion.candidates.end(),
                        [label](const lang::CompletionCandidate &candidate) {
                          return candidate.label == label;
                        });
  };

  const std::string localSource =
      "int calculate(int parameter) { int local = 1; int sink = loc; "
      "return parameter; }";
  const lang::CompletionResult localCompletion = complete(localSource, "loc");
  const auto local = findCandidate(localCompletion, "local");
  expect(local != localCompletion.candidates.end() &&
             local->kind == lang::CompletionCandidateKind::Variable &&
             local->detail == "int32_t local",
         "unqualified completion should use live semantic locals and types");
  expect(local != localCompletion.candidates.end() &&
             local->replacementRange.start == localSource.rfind("loc") &&
             local->replacementRange.end ==
                 localSource.rfind("loc") + std::string("loc").size(),
         "completion should replace exactly the identifier prefix");

  const std::string namespaceSource =
      "namespace math { uint64_t power(uint64_t base, uint64_t exponent) { "
      "return "
      "base; } float power(float base, float exponent) { return base; } } "
      "int main() { uint64_t result = math::po; return 0; }";
  const lang::CompletionResult namespaceCompletion =
      complete(namespaceSource, "po");
  const std::size_t powerOverloads = static_cast<std::size_t>(
      std::count_if(namespaceCompletion.candidates.begin(),
                    namespaceCompletion.candidates.end(),
                    [](const lang::CompletionCandidate &candidate) {
                      return candidate.label == "power";
                    }));
  const auto power = findCandidate(namespaceCompletion, "power");
  expect(powerOverloads == 2 && power != namespaceCompletion.candidates.end() &&
             power->detail.find("math::power") != std::string::npos &&
             power->snippet &&
             power->snippet->find("${1:base}") != std::string::npos,
         "namespace completion should preserve overloads, GTI signatures, and "
         "parameter snippets");

  const std::string incompleteDeclarationSource =
      "class Client { public: static int create() { return 1; } }; "
      "int unfinished() { auto client = Client::\n}";
  const lang::CompletionResult incompleteDeclarationCompletion =
      complete(incompleteDeclarationSource, "Client::");
  expect(findCandidate(incompleteDeclarationCompletion, "create") !=
             incompleteDeclarationCompletion.candidates.end(),
         "completion should retain a declaration whose initializer reaches "
         "the cursor before its semicolon is written");

  const std::string languageSurfaceSource =
      "interface Renderable { int render(); }; "
      "void relay<Args...>(Args... values) {} "
      "int main() { auto type = Ren; rel; return 0; }";
  const lang::CompletionResult interfaceCompletion =
      complete(languageSurfaceSource, "Ren");
  const auto renderable = findCandidate(interfaceCompletion, "Renderable");
  expect(renderable != interfaceCompletion.candidates.end() &&
             renderable->detail == "interface Renderable",
         "completion detail should preserve interface spelling");
  const lang::CompletionResult variadicCompletion =
      complete(languageSurfaceSource, "rel");
  const auto relay = findCandidate(variadicCompletion, "relay");
  expect(relay != variadicCompletion.candidates.end() &&
             relay->detail == "void relay<Args...>(Args... values)",
         "completion detail should preserve variadic-pack spelling");

  const std::string memberSource =
      "class Box { int hidden = 0; public: int read() { return this.hidden; } "
      "}; int main() { Box box{}; int result = box.rea; return 0; }";
  const lang::CompletionResult memberCompletion = complete(memberSource, "rea");
  expect(findCandidate(memberCompletion, "read") !=
                 memberCompletion.candidates.end() &&
             findCandidate(memberCompletion, "hidden") ==
                 memberCompletion.candidates.end(),
         "member completion should include accessible methods and hide private "
         "fields");

  const std::string enumSource =
      "enum class State { Ready, Running }; int main() { State state = "
      "State::Ru; return 0; }";
  const lang::CompletionResult enumCompletion = complete(enumSource, "Ru");
  const auto running = findCandidate(enumCompletion, "Running");
  expect(running != enumCompletion.candidates.end() &&
             running->kind == lang::CompletionCandidateKind::Enumerator &&
             std::all_of(enumCompletion.candidates.begin(),
                         enumCompletion.candidates.end(),
                         [](const lang::CompletionCandidate &candidate) {
                           return candidate.kind ==
                                  lang::CompletionCandidateKind::Enumerator;
                         }),
         "scoped-enum completion should filter and expose only enum members");
}

void testToolingOccurrenceOptOut() {
  const std::string source =
      "class Holder { int32_t value; public: Holder(int32_t value) : "
      "value(value) {} int32_t read() { return this.value; } };\n"
      "int main() { Holder holder{7}; return holder.read() - 7; }\n";

  const lang::FrontendResult tooling =
      lang::Frontend().analyze("occurrences-on.gti", source);
  expect(tooling.canGenerateCode(),
         "the tooling configuration should still compile");
  const lang::SourceUnitId unit = tooling.sourceGraph.entryUnit();
  expect(!tooling.semantics.database().occurrences(unit).empty(),
         "occurrences are recorded by default for editor queries");
  const std::size_t use = source.rfind("read");
  expect(tooling.semantics.database().findSymbolAt(unit, use + 1) != nullptr,
         "a recorded occurrence should resolve a symbol at its position");

  lang::FrontendOptions compileOnly;
  compileOnly.toolingOccurrences = false;
  const lang::FrontendResult compiled =
      lang::Frontend(compileOnly).analyze("occurrences-off.gti", source);
  expect(compiled.canGenerateCode(),
         "disabling occurrences must not affect code generation");
  expect(compiled.semantics.database()
             .occurrences(compiled.sourceGraph.entryUnit())
             .empty(),
         "a compile-only analysis should record no occurrences");
  expect(!compiled.semantics.database().symbols().empty(),
         "symbols stay recorded because HIR and the emitter resolve member "
         "identity through them");
  expect(compiled.semantics.expressionCount() ==
             tooling.semantics.expressionCount(),
         "occurrence recording must not change semantic analysis results");
}

void testReadOnlyReceiverDiagnostics() {
  const std::string invalidSource = R"(
struct Component {
  mut int offset = 0;
};

class Path {
public:
  void direct() {
    direct_value = 1;
  }

  void nested() {
    component.offset = 2;
  }

  void bind() {
    mut int& alias = component.offset;
  }

private:
  mut int direct_value = 0;
  mut Component component = Component();
};
)";
  const lang::FrontendResult invalid = lang::Frontend().analyze(
      "read-only-receiver-diagnostics.gti", invalidSource);
  const auto diagnosticAt = [&](std::string_view code, std::size_t offset) {
    return std::find_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                        [&](const lang::Diagnostic &diagnostic) {
                          return diagnostic.code == code &&
                                 diagnostic.primary.start == offset;
                        });
  };
  const std::size_t directUse = invalidSource.find("direct_value = 1");
  const std::size_t nestedUse = invalidSource.find("offset = 2");
  const std::size_t referenceUse = invalidSource.rfind("offset;");
  const auto direct = diagnosticAt("GTI-S2002", directUse);
  const auto nested = diagnosticAt("GTI-S2002", nestedUse);
  const auto reference = diagnosticAt("GTI-S2017", referenceUse);
  const auto hasReceiverContract = [&](const lang::Diagnostic &diagnostic,
                                       std::string_view method,
                                       std::size_t methodOffset,
                                       std::string_view root,
                                       std::size_t rootOffset) {
    return diagnostic.related.size() == 2 &&
           diagnostic.related[0].span.start == methodOffset &&
           diagnostic.related[0].span.end == methodOffset + method.size() &&
           diagnostic.related[0].message == "Method '" + std::string(method) +
                                                "' is read-only by default." &&
           diagnostic.related[1].span.start == rootOffset &&
           diagnostic.related[1].span.end == rootOffset + root.size() &&
           diagnostic.related[1].message ==
               "Field '" + std::string(root) +
                   "' is declared mutable, but it is writable only through "
                   "a mutable receiver." &&
           diagnostic.hints ==
               std::vector<std::string>{
                   "Add trailing 'mut' only if callers should require a "
                   "mutable 'Path'; otherwise avoid changing receiver state "
                   "in this method."} &&
           diagnostic.fixes.empty();
  };
  const std::size_t directDeclaration = invalidSource.find("direct_value = 0");
  const std::size_t componentDeclaration =
      invalidSource.find("component = Component");
  expect(!invalid.canGenerateCode() && direct != invalid.diagnostics.end() &&
             direct->primary.end ==
                 directUse + std::string("direct_value").size() &&
             direct->message ==
                 "Cannot modify field 'direct_value' because method 'direct' "
                 "has no trailing 'mut' receiver qualifier." &&
             hasReceiverContract(*direct, "direct",
                                 invalidSource.find("direct()"), "direct_value",
                                 directDeclaration),
         "a direct implicit field write should identify the read-only method "
         "and mutable field declaration");
  expect(
      nested != invalid.diagnostics.end() &&
          nested->primary.end == nestedUse + std::string("offset").size() &&
          nested->message ==
              "Cannot modify field 'offset' because method 'nested' has no "
              "trailing 'mut' receiver qualifier." &&
          hasReceiverContract(*nested, "nested", invalidSource.find("nested()"),
                              "component", componentDeclaration),
      "a nested receiver-rooted field write should retain its terminal field "
      "and root declaration");
  expect(reference != invalid.diagnostics.end() &&
             reference->primary.end ==
                 referenceUse + std::string("offset").size() &&
             reference->message ==
                 "Cannot bind a mutable reference to field 'offset' because "
                 "method 'bind' has no trailing 'mut' receiver qualifier." &&
             hasReceiverContract(*reference, "bind",
                                 invalidSource.find("bind()"), "component",
                                 componentDeclaration),
         "a receiver-rooted mutable-reference initializer should retain S2017 "
         "while explaining the missing receiver qualifier");
  expect(countDiagnosticContaining(invalid.diagnostics,
                                   "no trailing 'mut' receiver qualifier") ==
                 3 &&
             !hasDiagnostic(invalid.diagnostics,
                            "Cannot mutate through a read-only receiver"),
         "receiver-rooted failures should not cascade with the generic "
         "read-only receiver diagnostic");

  const lang::FrontendResult immutableField =
      lang::Frontend().analyze("immutable-receiver-field.gti", R"(
struct Component { int offset = 0; };
class Path {
public:
  void update() { component.offset = 1; }
private:
  mut Component component = Component();
};
)");
  expect(!immutableField.canGenerateCode() &&
             hasDiagnostic(immutableField.diagnostics, "Member is immutable") &&
             !hasDiagnostic(immutableField.diagnostics,
                            "no trailing 'mut' receiver qualifier"),
         "an immutable terminal field should keep field immutability "
         "precedence");

  const lang::FrontendResult unrelatedObject =
      lang::Frontend().analyze("unrelated-read-only-object.gti", R"(
struct Component { mut int offset = 0; };
void update(Component value) { value.offset = 1; }
)");
  expect(!unrelatedObject.canGenerateCode() &&
             hasDiagnostic(unrelatedObject.diagnostics,
                           "Cannot mutate through a read-only receiver") &&
             !hasDiagnostic(unrelatedObject.diagnostics,
                            "no trailing 'mut' receiver qualifier"),
         "an unrelated read-only object should not be mislabeled as the "
         "current method receiver");

  const lang::FrontendResult mutableReceiver =
      lang::Frontend().analyze("mutable-receiver-fields.gti", R"(
struct Component { mut int offset = 0; };
class Path {
public:
  void update() mut {
    direct_value = 1;
    component.offset = 2;
    mut int& alias = component.offset;
    alias = 3;
  }
private:
  mut int direct_value = 0;
  mut Component component = Component();
};
)");
  expect(mutableReceiver.canGenerateCode(),
         "adding trailing mut should preserve valid receiver mutation and "
         "mutable-reference semantics");
}

void testUnknownFunctionAttributeClassification() {
  const std::string source =
      "[[noretrun]]\nvoid stop() {}\nint main() { return 0; }\n";
  const auto result = lang::Frontend().analyze("unknown-attribute.gti", source);
  expect(!result.canGenerateCode() && result.diagnostics.size() == 1,
         "an unknown function attribute should have one owning diagnostic");
  if (result.diagnostics.size() != 1) {
    return;
  }
  const auto &diagnostic = result.diagnostics.front();
  expect(diagnostic.code == "GTI-S2099" &&
             diagnostic.phase == lang::DiagnosticPhase::Semantics &&
             diagnostic.severity == lang::DiagnosticSeverity::Error &&
             diagnostic.message.find("Unknown GTI function attribute 'noretrun'") !=
                 std::string::npos &&
             diagnostic.primary.source ==
                 std::filesystem::absolute("unknown-attribute.gti").string() &&
             diagnostic.primary.start == 2 && diagnostic.primary.end == 10 &&
             diagnostic.primary.line == 1,
         "attribute classification should retain its semantic code and exact span");
  expect(findTopLevelFunction(result.program, "main") != nullptr,
         "an unknown attribute must preserve following declarations");
  const auto known = lang::Frontend().analyze(
      "known-attribute.gti", "[[discardable]] int compute() { return 4; }");
  expect(known.canGenerateCode(), "recognized function attributes remain valid");
  const auto classAttribute = lang::Frontend().analyze(
      "class-attribute.gti", "[[not_an_attribute]] struct Record {}; ");
  expect(!classAttribute.canGenerateCode() &&
             !hasDiagnostic(classAttribute.diagnostics,
                            "Unknown GTI function attribute"),
         "class attributes retain their owning class validation");
}

void testUnknownTypeCascadeSuppression() {
  const auto result = lang::Frontend().analyze("unknown-cascade.gti", R"(
int main() {
  Missing value{};
  auto result = missing + "text";
  int independent = "text";
  return 0;
}
)");
  expect(!result.canGenerateCode(), "unknown operands must remain rejected");
  expect(hasDiagnostic(result.diagnostics, "Unknown type 'Missing'") &&
             hasDiagnostic(result.diagnostics, "Undefined name 'missing'") &&
             hasDiagnostic(result.diagnostics, "Cannot initialize 'independent'"),
         "poison recovery retains lookup errors and independent type errors");
  expect(!hasDiagnostic(result.diagnostics, "of type 'unknown'") &&
             !hasDiagnostic(result.diagnostics, "and 'unknown'"),
         "poison types must not create initializer or operator cascades");
  const auto numeric = lang::Frontend().analyze(
      "numeric-mismatch.gti", "int main() { return 1 + true; }");
  expect(!numeric.canGenerateCode() &&
             hasDiagnostic(numeric.diagnostics, "requires numeric operands"),
         "known operand mismatches retain their diagnostic");
}

constexpr NamedTest kTests[] = {
    {"testUnknownTypeCascadeSuppression", testUnknownTypeCascadeSuppression},
    {"testUnknownFunctionAttributeClassification",
     testUnknownFunctionAttributeClassification},
    {"testSemanticVisitorRequestReuse", testSemanticVisitorRequestReuse},
    {"testSemanticDiagnostics", testSemanticDiagnostics},
    {"testOwnershipAndMissingIncludeDiagnostics",
     testOwnershipAndMissingIncludeDiagnostics},
    {"testDiagnosticFoundation", testDiagnosticFoundation},
    {"testCppReservedIdentifiers", testCppReservedIdentifiers},
    {"testDefaultImmutability", testDefaultImmutability},
    {"testDefaultNodiscard", testDefaultNodiscard},
    {"testAdvisoryDiagnostics", testAdvisoryDiagnostics},
    {"testPrintIsAnIdentifier", testPrintIsAnIdentifier},
    {"testFormatting", testFormatting},
    {"testLanguageQueries", testLanguageQueries},
    {"testToolingOccurrenceOptOut", testToolingOccurrenceOptOut},
    {"testReadOnlyReceiverDiagnostics", testReadOnlyReceiverDiagnostics},
};

} // namespace

int main(int argc, char **argv) {
  lang::installCrashHandlers("gti_compiler_diagnostics_tests");
  return runNamedTests(argc, argv, kTests, "diagnostics");
}
