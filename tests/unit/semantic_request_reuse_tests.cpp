// SemanticVisitor reuse across requests: identities, AST-backed facts and
// diagnostics never leak from one request into the next. Library API with no
// source-level observable. (Moved unchanged from
// the former compiler_diagnostics_tests.cpp.)
#include "gti/lexer.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"
#include "gti/source_loader.h"
#include "unit_test.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

const lang::FunctionDecl *findTopLevelFunction(const lang::Program &program,
                                               const std::string &name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *function =
        dynamic_cast<const lang::FunctionDecl *>(declaration.get());
    if (function != nullptr && function->name().lexeme == name)
      return function;
  }
  return nullptr;
}

void testSemanticVisitorRequestReuse() {
  // Checked borrow ancestry uses exact source-unit/declaration identities.
  // Supply those facts even when exercising the lower-level visitor directly.
  lang::SourceLoader loader;
  const auto graph = loader.load("semantic-request-reuse.gti", std::string{});
  expect(!loader.hadError(),
         "semantic reuse fixture should have a source identity");
  const std::string sourcePath =
      graph.findUnit(graph.entryUnit())->path.string();
  const auto parseProgram = [&](std::string source) {
    lang::Lexer lexer;
    std::vector<lang::Token> tokens = lexer.scan(std::move(source), sourcePath);
    expect(!lexer.hadError(), "semantic reuse fixture should lex");
    lang::Parser parser(std::move(tokens));
    lang::Program program = parser.parse();
    expect(!parser.hadError(), "semantic reuse fixture should parse");
    return program;
  };
  const auto parseExpression = [&](std::string source) {
    lang::Lexer lexer;
    std::vector<lang::Token> tokens = lexer.scan(std::move(source), sourcePath);
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

  lang::SemanticVisitor semantic(lang::TargetInfo::host(), &graph);
  expect(semantic.check(first),
         "the first request in a reusable semantic visitor should pass");
  const lang::FunctionInfo *firstInfo =
      firstFunction != nullptr ? semantic.model().findFunction(*firstFunction)
                               : nullptr;
  expect(firstInfo != nullptr && firstInfo->id == 1,
         "the first request should allocate its first function identity");
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
             semantic.errors().empty() &&
             semantic.expressionType() == lang::SemanticType::Bool,
         "a later expression request should clear diagnostics while "
         "publishing its own exact type");
}

// Moved unchanged from raw_borrow_tests.cpp (its GTI-source cases became
// tests/ui/unsafe/raw_borrow files): a reused visitor discards raw-reference
// assertion facts and raw borrow call uses from the previous request.
void assertionReset() {
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(R"(
unsafe int&<a> observe<lifetime a>(const int* address, int&<a> owner) {
  unsafe { return *address; }
}
int main() { return 0; }
)"));
  auto program = parser.parse();
  const auto *function =
      lang::stmtAs<lang::FunctionDecl>(program.declarations().front().get());
  const auto *block = function
                          ? lang::stmtAs<lang::BlockStmt>(
                                function->body()->statements().front().get())
                          : nullptr;
  const auto *returned =
      block ? lang::stmtAs<lang::ReturnStmt>(block->statements().front().get())
            : nullptr;
  expect(returned != nullptr, "reset regression has a raw assertion return");
  if (!returned)
    return;
  lang::SemanticVisitor semantic;
  expect(semantic.check(program) && semantic.model().findRawReferenceAssertion(
                                        *returned->value()) != nullptr,
         "first request records its raw assertion");
  lang::Lexer nextLexer;
  lang::Parser nextParser(nextLexer.scan("int main() { return 0; }"));
  auto next = nextParser.parse();
  expect(semantic.check(next) && semantic.model().findRawReferenceAssertion(
                                     *returned->value()) == nullptr,
         "semantic request reset discards raw assertion facts");
}

void repeatedSemanticRequest() {
  lang::Lexer firstLexer;
  lang::Parser firstParser(firstLexer.scan(R"(
unsafe int&<a> observe<lifetime a>(const int* pointer, int&<a> witness) {
  unsafe { return *pointer; }
}
int main() { int value = 7;
  unsafe { int& result = observe(&value, value); return result; }
}
)"));
  auto first = firstParser.parse();
  lang::Lexer secondLexer;
  lang::Parser secondParser(secondLexer.scan("int main() { return 0; }"));
  auto second = secondParser.parse();
  lang::SemanticVisitor semantic;
  expect(semantic.check(first),
         "first reused semantic request records raw call uses");
  expect(semantic.check(second),
         "semantic request reset clears raw borrow call uses");
}

} // namespace

GTI_UNIT_TEST(semantic_request_reuse) {
  testSemanticVisitorRequestReuse();
  assertionReset();
  repeatedSemanticRequest();
  return failures == 0 ? 0 : 1;
}
