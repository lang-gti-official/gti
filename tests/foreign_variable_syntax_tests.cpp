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
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testDeclarationsAndFormatting() {
  const std::string source =
      R"(extern "C"{mut c_int counter;c_int limit;mut Callback slot;[[link_name("native_count")]]mut c_int renamed;c_int read_counter();})";
  const lang::Formatter formatter;
  const std::string formatted = formatter.format(source);
  expect(formatter.format(formatted) == formatted,
         "foreign variable formatting should be idempotent");
  for (const std::string &input : {source, formatted}) {
    lang::Lexer lexer;
    lang::Parser parser(lexer.scan(input, "foreign-variables.gti"));
    const auto program = parser.parse();
    expect(!lexer.hadError() && parser.errors().empty(),
           "foreign declarations should parse before and after formatting");
    expect(program.declarations().size() == 1,
           "C linkage block should remain one declaration");
    const auto *block =
        lang::stmtAs<lang::ExternCDecl>(program.declarations()[0].get());
    expect(block && block->declarations().size() == 5,
           "C linkage block should retain variable and function declarations");
    for (std::size_t index = 0; index < 4; ++index) {
      const auto *variable =
          lang::stmtAs<lang::VariableDecl>(block->declarations()[index].get());
      expect(variable && variable->linkage() == lang::LanguageLinkage::C &&
                 !variable->initializer() &&
                 variable->isMutable() == (index != 1),
             "foreign AST identity must retain linkage, constness and no "
             "initializer");
      expect(variable->nativeLinkName().has_value() == (index == 3),
             "only the renamed declaration should carry a native symbol");
      const auto clone = lang::cloneStmt(*variable);
      const auto *copy =
          lang::stmtAs<lang::VariableDecl>(clone.statement.get());
      expect(copy && copy->linkage() == variable->linkage() &&
                 copy->nativeLinkName().has_value() ==
                     variable->nativeLinkName().has_value(),
             "AST cloning must preserve foreign variable identity metadata");
      if (index == 3) {
        expect(variable->nativeLinkName()->name() == "native_count",
               "foreign declaration must retain exact native link name");
      }
    }
    const auto *function =
        lang::stmtAs<lang::FunctionDecl>(block->declarations()[4].get());
    expect(function && function->linkage() == lang::LanguageLinkage::C,
           "foreign variables must preserve existing C function syntax");
  }
}

void testInitializerRejectionAndRecovery() {
  for (const std::string_view initializer : {"= 1", "{1}"}) {
    const std::string source = "extern \"C\" { mut c_int invalid" +
                               std::string(initializer) +
                               "; mut c_int recovered; }";
    lang::Lexer lexer;
    lang::Parser parser(lexer.scan(source, "foreign-initializer.gti"));
    const auto program = parser.parse();
    expect(parser.errors().size() == 1 &&
               parser.errors()[0].message ==
                   "A foreign variable declaration cannot have an initializer.",
           "foreign definitions must receive a precise initializer diagnostic");
    expect(program.declarations().size() == 1,
           "invalid foreign initializer should preserve its linkage block");
    const auto *block =
        lang::stmtAs<lang::ExternCDecl>(program.declarations()[0].get());
    expect(block && block->declarations().size() == 1,
           "parser should recover to the next foreign declaration");
    const auto *recovered =
        lang::stmtAs<lang::VariableDecl>(block->declarations()[0].get());
    expect(recovered && recovered->name().lexeme == "recovered" &&
               recovered->linkage() == lang::LanguageLinkage::C,
           "recovered declaration should preserve foreign identity");
  }
}
} // namespace

int main() {
  testDeclarationsAndFormatting();
  testInitializerRejectionAndRecovery();
  std::cout << "Foreign variable syntax tests passed\n";
}
