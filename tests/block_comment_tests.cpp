#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/lexer.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

const lang::Token *findToken(const std::vector<lang::Token> &tokens,
                             std::string_view lexeme) {
  const auto found =
      std::find_if(tokens.begin(), tokens.end(), [&](const lang::Token &token) {
        return token.lexeme == lexeme;
      });
  return found == tokens.end() ? nullptr : &*found;
}

void testAcceptedBlockComments() {
  const std::string source = R"(/**/
/* preamble */
int /* result type */ main() {
  /* first line
     hidden identifier */
  int value = 8 / 2 * 3;
  return value;
}
)";

  lang::Lexer lexer;
  const std::vector<lang::Token> tokens =
      lexer.scan(source, "block-comments.gti");
  expect(!lexer.hadError(), "closed block comments should lex successfully");
  expect(findToken(tokens, "hidden") == nullptr &&
             findToken(tokens, "identifier") == nullptr,
         "block-comment contents should not become language tokens");

  const lang::Token *returnToken = findToken(tokens, "return");
  const int expectedLine =
      returnToken == nullptr
          ? 0
          : 1 + static_cast<int>(
                    std::count(source.begin(),
                               source.begin() + returnToken->position, '\n'));
  expect(returnToken != nullptr && returnToken->line == expectedLine &&
             returnToken->line == 7,
         "multiline comments should preserve following token line numbers");

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("block-comments.gti", source);
  expect(frontend.canGenerateCode(),
         "block comments should be accepted throughout the frontend");

  lang::Lexer literalLexer;
  const std::vector<lang::Token> literalTokens = literalLexer.scan(
      R"("/* string */" '/' /* outer /* inner */ int value = 1;)");
  expect(!literalLexer.hadError() && !literalTokens.empty() &&
             literalTokens.front().kind == lang::TokenKind::STRING_LITERAL &&
             findToken(literalTokens, "int") != nullptr,
         "comment delimiters in literals should remain literal text and block "
         "comments should use the first closing delimiter");
}

void testUnterminatedBlockCommentDiagnostic() {
  const std::string source = "int value = 1;\n  /* unfinished\nstill open";
  const std::size_t opening = source.find("/*");

  lang::Lexer lexer;
  const std::vector<lang::Token> tokens =
      lexer.scan(source, "unterminated-block-comment.gti");
  expect(lexer.errors().size() == 1,
         "one unterminated block comment should produce one diagnostic");
  if (lexer.errors().size() == 1) {
    const lang::Diagnostic &diagnostic = lexer.errors().front();
    expect(diagnostic.code == "GTI-L0011" &&
               diagnostic.phase == lang::DiagnosticPhase::Lexing &&
               diagnostic.severity == lang::DiagnosticSeverity::Error &&
               diagnostic.message == "Unterminated block comment." &&
               diagnostic.primary.source == "unterminated-block-comment.gti" &&
               diagnostic.primary.start == opening &&
               diagnostic.primary.end == opening + 2 &&
               diagnostic.primary.line == 2 && diagnostic.related.empty() &&
               diagnostic.hints.empty() && diagnostic.fixes.empty(),
           "the unterminated-comment diagnostic should use the stable code "
           "and exact opening-delimiter span");
  }
  expect(tokens.size() == 6 &&
             tokens.back().kind == lang::TokenKind::END_OF_FILE,
         "unterminated block-comment recovery should retain preceding tokens "
         "and finish at EOF");

  const lang::FrontendResult frontend =
      lang::Frontend().analyze("unterminated-block-comment.gti", source);
  const std::size_t diagnostics = static_cast<std::size_t>(
      std::count_if(frontend.diagnostics.begin(), frontend.diagnostics.end(),
                    [](const lang::Diagnostic &diagnostic) {
                      return diagnostic.code == "GTI-L0011";
                    }));
  expect(!frontend.sourceValid && !frontend.canGenerateCode() &&
             diagnostics == 1,
         "an unterminated block comment should stop generation without parser "
         "cascades");
}

void testBlockCommentFormatting() {
  const std::string source = "int/* result type */main(){\n"
                             "/*\n * first line\n */\n"
                             "int value=1;return value;/*done*/\n}";
  const std::string formatted = lang::Formatter().format(source);
  const std::string expected = R"(int /* result type */ main() {
  /*
   * first line
   */
  int value = 1;
  return value; /*done*/
}
)";
  expect(formatted == expected,
         "the formatter should preserve inline and multiline block comments");
  const std::string reformatted = lang::Formatter().format(formatted);
  if (reformatted != formatted) {
    std::cerr << "Reformatted block-comment output was:\n" << reformatted;
  }
  expect(reformatted == formatted,
         "block-comment formatting should be idempotent");
}

} // namespace

int main() {
  testAcceptedBlockComments();
  testUnterminatedBlockCommentDiagnostic();
  testBlockCommentFormatting();

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "Block comment tests passed\n";
  return 0;
}
