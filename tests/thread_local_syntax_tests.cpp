#include "gti/ast_clone.h"
#include "gti/formatter.h"
#include "gti/lexer.h"
#include "gti/parser.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void declarationsAndClones() {
  const std::string source = R"(
thread_local mut int counter = 0;
static thread_local int first = 1;
thread_local static int second = 2;
class Store {
  static thread_local mut Store* current = nullptr;
};
int main() {
  thread_local mut int local = 0;
  static thread_local int another = 1;
  return local;
}
)";
  const lang::Formatter formatter;
  const std::string formatted = formatter.format(source);
  expect(formatter.format(formatted) == formatted,
         "TLS declaration formatting is idempotent");
  for (const auto &input : {source, formatted}) {
    lang::Lexer lexer;
    const auto tokens = lexer.scan(input, "thread-local-syntax.gti");
    expect(!lexer.hadError(), "thread_local is a source keyword");
    lang::Parser parser(tokens);
    const auto program = parser.parse();
    for (const auto &diagnostic : parser.errors())
      std::cerr << diagnostic.message << '\n';
    expect(parser.errors().empty() && program.declarations().size() == 5,
           "namespace, static field and block TLS declarations parse");
    const auto check = [&](const lang::Stmt &statement, bool isStatic) {
      const auto *variable = lang::stmtAs<lang::VariableDecl>(&statement);
      expect(variable && variable->isThreadLocal() &&
                 variable->isStatic() == isStatic &&
                 variable->threadLocalKeyword()->kind ==
                     lang::TokenKind::THREAD_LOCAL,
             "AST retains thread storage independently of static linkage");
      const auto clone = lang::cloneStmt(*variable, 37);
      const auto *copy =
          lang::stmtAs<lang::VariableDecl>(clone.statement.get());
      expect(copy && copy->isThreadLocal() && copy->isStatic() == isStatic &&
                 copy->threadLocalKeyword()->expansion == 37 &&
                 copy->threadLocalKeyword()->position ==
                     variable->threadLocalKeyword()->position,
             "cloning retains storage duration and remaps token identity");
    };
    check(*program.declarations()[0], false);
    check(*program.declarations()[1], true);
    check(*program.declarations()[2], true);
    const auto *owner =
        lang::stmtAs<lang::ClassDecl>(program.declarations()[3].get());
    expect(owner && owner->members().size() == 1,
           "TLS field retains its owner");
    check(*owner->members().front(), true);
    const auto *function =
        lang::stmtAs<lang::FunctionDecl>(program.declarations()[4].get());
    expect(function && function->body(), "block TLS retains its lexical scope");
    check(*function->body()->statements()[0], false);
    check(*function->body()->statements()[1], true);
  }
}

void invalidModifiersAndRecovery() {
  for (const std::string source :
       {"thread_local thread_local int invalid; int recovered;",
        "static static thread_local int invalid; int recovered;",
        "thread_local void invalid() {} int recovered;"}) {
    lang::Lexer lexer;
    lang::Parser parser(lexer.scan(source, "thread-local-invalid.gti"));
    const auto program = parser.parse();
    expect(!parser.errors().empty() &&
               parser.errors().front().code == "GTI-P0001" &&
               parser.errors().front().primary.source ==
                   "thread-local-invalid.gti",
           "invalid TLS declaration has a source parser diagnostic");
    bool recovered = false;
    for (const auto &statement : program.declarations())
      if (const auto *variable =
              lang::stmtAs<lang::VariableDecl>(statement.get()))
        recovered |= variable->name().lexeme == "recovered";
    expect(recovered, "bad TLS modifiers preserve following declarations");
  }
}
} // namespace

int main() {
  declarationsAndClones();
  invalidModifiersAndRecovery();
}
