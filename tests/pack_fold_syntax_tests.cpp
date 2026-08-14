#include "gti/ast_printer.h"
#include "gti/formatter.h"
#include "gti/lexer.h"
#include "gti/parser.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

const lang::FunctionDecl *findFunction(const lang::Program &program,
                                       std::string_view name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *function =
        dynamic_cast<const lang::FunctionDecl *>(declaration.get());
    if (function != nullptr && function->name().lexeme == name) {
      return function;
    }
  }
  return nullptr;
}

void testPackFoldAst() {
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(R"(
void emit_each<Args...>(Args... values) {
  (emit(values), ...);
}
)"));
  const lang::Program program = parser.parse();
  const lang::FunctionDecl *function = findFunction(program, "emit_each");
  const lang::ExpressionStmt *statement =
      function != nullptr && function->body() != nullptr &&
              !function->body()->statements().empty()
          ? dynamic_cast<const lang::ExpressionStmt *>(
                function->body()->statements().front().get())
          : nullptr;
  const auto *fold =
      statement == nullptr
          ? nullptr
          : dynamic_cast<const lang::PackFold *>(statement->expression().get());
  const auto *call =
      fold == nullptr ? nullptr
                      : dynamic_cast<const lang::Call *>(fold->pattern().get());

  expect(!lexer.hadError() && !parser.hadError() && fold != nullptr &&
             call != nullptr,
         "a bounded comma fold should retain its call pattern as PackFold");
  if (fold != nullptr) {
    expect(fold->leftParen().lexeme == "(" && fold->comma().lexeme == "," &&
               fold->ellipsis().lexeme == "..." &&
               fold->rightParen().lexeme == ")",
           "the pack-fold AST should retain each delimiting token");
    expect(lang::AstPrinter().print(*fold) == "(pack-fold (call emit values))",
           "the AST printer should expose the fold and its call pattern");
  }

  lang::Parser commaParser(lexer.scan("(left, right)"));
  const lang::ExprPtr commaExpression = commaParser.parseExpression();
  expect(!commaParser.hadError() && commaExpression != nullptr &&
             dynamic_cast<const lang::Grouping *>(commaExpression.get()) !=
                 nullptr &&
             lang::AstPrinter().print(*commaExpression) ==
                 "(group (, left right))",
         "ordinary parenthesized comma expressions should remain unchanged");
}

void testPackFoldRecovery() {
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(R"(
void invalid<Args...>(Args... values) {
  (values, ...);
  emit(values...);
}

int intact() { return 0; }
)"));
  const lang::Program program = parser.parse();

  bool foundDiagnostic = false;
  for (const lang::ParseDiagnostic &diagnostic : parser.errors()) {
    if (diagnostic.message.find(
            "A pack fold pattern must be a function or method call") !=
        std::string::npos) {
      foundDiagnostic = true;
      break;
    }
  }

  const lang::FunctionDecl *invalid = findFunction(program, "invalid");
  const lang::FunctionDecl *intact = findFunction(program, "intact");
  expect(parser.hadError() && foundDiagnostic,
         "a non-call pack-fold pattern should receive a targeted parse "
         "diagnostic");
  expect(invalid != nullptr && invalid->body() != nullptr &&
             invalid->body()->statements().size() == 1 && intact != nullptr,
         "pack-fold recovery should preserve later statements and "
         "declarations");
}

void testPackFoldFormatting() {
  const std::string formatted = lang::Formatter().format(
      "void emit_each<Args...>(Args... values){(emit(values),...);}");
  const std::string expected = "void emit_each<Args...>(Args... values) {\n"
                               "  (emit(values), ...);\n"
                               "}\n";
  if (formatted != expected) {
    std::cerr << "pack-fold formatter produced:\n" << formatted;
  }
  expect(formatted == expected &&
             lang::Formatter().format(formatted) == formatted,
         "the formatter should normalize bounded comma folds idempotently");
}

} // namespace

int main() {
  testPackFoldAst();
  testPackFoldRecovery();
  testPackFoldFormatting();
  return failures == 0 ? 0 : 1;
}
