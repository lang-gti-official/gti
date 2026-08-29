#include "gti/diagnostic.h"
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

void testBomAndUnicodeContexts() {
  const std::string bom = "\xEF\xBB\xBF";
  const std::string source = bom + R"(int main() {
  // UTF-8 comments may contain café and 😀.
  std::println("héllo 😀");
  return 0;
}
)";

  lang::Lexer lexer;
  const std::vector<lang::Token> tokens =
      lexer.scan(source, "unicode-contexts.gti");
  expect(!lexer.hadError(),
         "a leading BOM and UTF-8 in comments and strings should lex");
  expect(lexer.sourceText() == source,
         "lexing should preserve the exact source bytes");
  expect(!tokens.empty() && tokens.front().kind == lang::TokenKind::INT &&
             tokens.front().position == bom.size() && tokens.front().line == 1,
         "the BOM should be ignored without changing following byte offsets");

  lang::SourceManager sources;
  sources.set("unicode-contexts.gti", source);
  const lang::SourceLocation location =
      sources.locate(lang::tokenSpan(tokens.front()));
  expect(location.line == 1 && location.column == 2,
         "diagnostic columns should count the BOM as one UTF-8 scalar");

  const std::string scalarLine = "😀value\n";
  sources.set("scalar-column.gti", scalarLine);
  const lang::SourceLocation scalarLocation = sources.locate(
      {"scalar-column.gti", scalarLine.find("value"), scalarLine.size(), 1});
  expect(scalarLocation.column == 2,
         "source columns should count Unicode scalar values rather than bytes");
}

void testInvalidUtf8StopsAtTheLexicalBoundary() {
  const std::vector<std::string> invalidSources{
      std::string("\x80", 1),
      std::string("\xC0\xAF", 2),
      std::string("\xE2\x82", 2),
      std::string("\xED\xA0\x80", 3),
      std::string("\xF4\x90\x80\x80", 4),
  };

  for (const std::string &source : invalidSources) {
    lang::Lexer lexer;
    const std::vector<lang::Token> tokens =
        lexer.scan(source, "invalid-utf8.gti");
    expect(lexer.errors().size() == 1 &&
               lexer.errors().front().code == "GTI-L0013" &&
               lexer.errors().front().message ==
                   "Source text is not valid UTF-8." &&
               lexer.errors().front().primary.start == 0 &&
               lexer.errors().front().primary.end == 1 && tokens.size() == 1 &&
               tokens.front().kind == lang::TokenKind::END_OF_FILE,
           "each invalid UTF-8 source should produce one exact lexical error");
  }

  const std::string source =
      std::string("int value = 0;\n") + std::string("\xC0\xAF", 2);
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("invalid-frontend.gti", source);
  expect(!frontend.sourceValid && !frontend.canGenerateCode() &&
             frontend.diagnostics.size() == 1 &&
             frontend.diagnostics.front().code == "GTI-L0013",
         "invalid UTF-8 should not cascade into parser or semantic errors");
}

void testAsciiTokenBoundary() {
  const std::string source = "int café = 0;";
  lang::Lexer lexer;
  const std::vector<lang::Token> tokens =
      lexer.scan(source, "ascii-identifiers.gti");
  const std::size_t accent = source.find("é");
  expect(lexer.errors().size() == 1 &&
             lexer.errors().front().code == "GTI-L0015" &&
             lexer.errors().front().primary.start == accent &&
             lexer.errors().front().primary.end == accent + 2 &&
             findToken(tokens, "caf") != nullptr,
         "a non-ASCII identifier scalar should receive one whole-scalar error");

  const std::string misplacedBom = "int \xEF\xBB\xBFvalue = 0;";
  lang::Lexer misplacedLexer;
  static_cast<void>(misplacedLexer.scan(misplacedBom, "misplaced-bom.gti"));
  expect(misplacedLexer.errors().size() == 1 &&
             misplacedLexer.errors().front().code == "GTI-L0015" &&
             misplacedLexer.errors().front().primary.start == 4 &&
             misplacedLexer.errors().front().primary.end == 7,
         "a BOM away from byte zero should be rejected as non-ASCII syntax");
}

void testNewlineContractAndLocations() {
  const std::string crlf =
      "int main() {\r\n  // café 😀\r\n  return 0;\r\n}\r\n";
  lang::Lexer lexer;
  const std::vector<lang::Token> tokens = lexer.scan(crlf, "crlf.gti");
  const lang::Token *returnToken = findToken(tokens, "return");
  expect(!lexer.hadError() && returnToken != nullptr && returnToken->line == 3,
         "CRLF should count as one logical newline");

  lang::SourceManager sources;
  sources.set("crlf.gti", crlf);
  const lang::SourceLocation location =
      sources.locate(lang::tokenSpan(*returnToken));
  expect(location.line == 3 && location.column == 3 &&
             sources.line(lang::tokenSpan(*returnToken)) == "  return 0;",
         "source locations should exclude both CRLF bytes from line text");

  const std::string loneCr = "int main() {\r  return 0;\n}\n";
  lang::Lexer loneCrLexer;
  const std::vector<lang::Token> loneCrTokens =
      loneCrLexer.scan(loneCr, "lone-cr.gti");
  const std::size_t carriageReturn = loneCr.find('\r');
  expect(loneCrLexer.errors().size() == 1 &&
             loneCrLexer.errors().front().code == "GTI-L0014" &&
             loneCrLexer.errors().front().primary.start == carriageReturn &&
             loneCrLexer.errors().front().primary.end == carriageReturn + 1 &&
             loneCrTokens.size() == 1 &&
             loneCrTokens.front().kind == lang::TokenKind::END_OF_FILE,
         "a lone carriage return should stop at the lexical boundary");
}

void testFormattingPreservesAcceptedText() {
  const std::string bom = "\xEF\xBB\xBF";
  const std::string source =
      bom + "int main(){\r\n// café 😀\r\n"
            "std::println(\"héllo 😀\");\r\nreturn 0;\r\n}\r\n";
  const std::string formatted = lang::Formatter().format(source);
  expect(formatted.starts_with(bom) &&
             formatted.find('\r') == std::string::npos &&
             formatted.find("café 😀") != std::string::npos &&
             formatted.find("héllo 😀") != std::string::npos,
         "formatting should preserve BOM and Unicode content while using LF");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatted UTF-8 source should be idempotent");

  const std::string invalid("int main() {}\n\x80", 15);
  expect(lang::Formatter().format(invalid) == invalid,
         "the formatter should leave invalid UTF-8 unchanged");
  const std::string loneCr = "int main() {\rreturn 0;}";
  expect(lang::Formatter().format(loneCr) == loneCr,
         "the formatter should leave invalid newline spelling unchanged");
  const std::string nonAsciiSyntax = "int café=0;";
  expect(lang::Formatter().format(nonAsciiSyntax) == nonAsciiSyntax,
         "the formatter should not rewrite rejected non-ASCII syntax");
}

} // namespace

int main() {
  testBomAndUnicodeContexts();
  testInvalidUtf8StopsAtTheLexicalBoundary();
  testAsciiTokenBoundary();
  testNewlineContractAndLocations();
  testFormattingPreservesAcceptedText();

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "Source text tests passed\n";
  return 0;
}
