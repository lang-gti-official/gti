// Lexer, parser and AstPrinter checks at the API level: literal escape
// decoding and its lexical diagnostics, operator precedence and the AST
// spellings of lambdas, pointer types, layout queries, direct initializers,
// assignments and unary operators. AstPrinter is a library API with no
// source-level observable. (Restored from the former
// compiler_constexpr_tests.cpp, layout_query_tests.cpp,
// raw_pointer_tests.cpp and compiler_declarations_tests.cpp, whose GTI
// sources became tests/ui cases; minimally adapted to parse expressions
// directly.)
#include "gti/ast_printer.h"
#include "gti/lexer.h"
#include "gti/parser.h"
#include "unit_test.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::size_t countCode(const std::vector<lang::Diagnostic> &diagnostics,
                      std::string_view code) {
  return static_cast<std::size_t>(
      std::count_if(diagnostics.begin(), diagnostics.end(),
                    [&](const lang::Diagnostic &diagnostic) {
                      return diagnostic.code == code;
                    }));
}

// The printed AST of one expression, or "<error>" when it does not parse.
std::string printed(lang::Lexer &lexer, std::string source) {
  lang::Parser parser(lexer.scan(std::move(source)));
  const lang::ExprPtr expression = parser.parseExpression();
  if (expression == nullptr || parser.hadError())
    return "<error>";
  return lang::AstPrinter().print(*expression);
}

void characterAndStringEscapes() {
  lang::Lexer lexer;
  expect(printed(lexer, "'a'") == "'a'",
         "character literals should retain their source-level AST spelling");

  (void)lexer.scan("char empty = ''; char many = 'ab'; "
                   "char escape = '\\q'; char open = 'x\n");
  expect(countCode(lexer.errors(), "GTI-L0010") == 2 &&
             countCode(lexer.errors(), "GTI-L0005") == 1 &&
             countCode(lexer.errors(), "GTI-L0009") == 1,
         "invalid character widths, escapes, and termination should have "
         "focused lexical diagnostics");

  // Control, fixed-width hexadecimal and Unicode escapes decode to code units;
  // \u and \U produce UTF-8.
  const auto literalTexts = [&](std::string sourceText) {
    std::vector<std::string> texts;
    for (const lang::Token &token : lexer.scan(std::move(sourceText)))
      if (const auto *text = std::get_if<std::string>(&token.literal))
        texts.push_back(*text);
    return texts;
  };
  const std::vector<std::string> escaped = literalTexts(
      R"(auto a = "\a\b\e\f\v"; auto b = "\x41\x7e\x00\xFF\x4142";
         auto c = "\u00e9\u20AC\U0001F600\u0041"; auto d = "it\'s";)");
  expect(lexer.errors().empty() && escaped.size() == 4 &&
             escaped[0] == "\a\b\x1b\f\v" &&
             escaped[1] == std::string("A~\0\xff"
                                       "A42",
                                       7) &&
             escaped[2] == "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"
                           "A" &&
             escaped[3] == "it's",
         "control, two-digit hexadecimal and Unicode escapes should decode "
         "to the exact code units");

  std::vector<std::uint8_t> characterValues;
  for (const lang::Token &token :
       lexer.scan(R"(char a = '\b'; char b = '\x41'; char c = '\e';
                     char d = '\''; char e = '\\'; char f = '\u0041';)"))
    if (const auto *character =
            std::get_if<lang::CharacterLiteral>(&token.literal))
      characterValues.push_back(character->value);
  expect(lexer.errors().empty() &&
             characterValues ==
                 std::vector<std::uint8_t>{8, 0x41, 0x1b, 0x27, 0x5c, 0x41},
         "character literals should accept escapes that decode to one code "
         "unit");

  const std::string malformed =
      R"(auto a = "\x4"; auto b = "\xZZ"; auto c = "\u12"; auto d = "\uD800";
         auto e = "\U00110000"; char f = '\u00e9';)";
  (void)lexer.scan(malformed);
  const auto spanOf = [&](std::string_view needle, std::size_t length) {
    const std::size_t at = malformed.find(needle);
    return std::pair{at, at + length};
  };
  const std::vector<std::pair<std::size_t, std::size_t>> expectedSpans{
      spanOf("\\x4\"", 3), spanOf("\\xZZ", 2), spanOf("\\u12", 4),
      spanOf("\\uD800", 6), spanOf("\\U00110000", 10)};
  bool spansMatch = countCode(lexer.errors(), "GTI-L0005") == 5 &&
                    countCode(lexer.errors(), "GTI-L0010") == 1;
  for (std::size_t index = 0; spansMatch && index < expectedSpans.size();
       ++index)
    spansMatch =
        lexer.errors()[index].primary.start == expectedSpans[index].first &&
        lexer.errors()[index].primary.end == expectedSpans[index].second;
  expect(spansMatch, "malformed escapes should report the exact escape span, "
                     "and a multi-byte character literal its width");
}

void operatorPrecedence() {
  lang::Lexer lexer;
  expect(printed(lexer, "true || false && false") ==
             "(|| true (&& false false))",
         "symbolic logical operators should use C++ precedence");
  expect(printed(lexer, "true or false and false") ==
             "(or true (and false false))",
         "word logical operators should retain their existing precedence");
  expect(printed(lexer, "1 | 2 ^ 3 & 4") == "(| 1 (^ 2 (& 3 4)))",
         "bitwise operators should follow C++ precedence");
  expect(printed(lexer, "1 < 2 << 3 + 4 % 2") == "(< 1 (<< 2 (+ 3 (% 4 2))))",
         "modulo and shifts should integrate with arithmetic precedence");
  expect(printed(lexer, "~1 * 2") == "(* (~ 1) 2)",
         "bitwise complement should bind as a unary operator");
  expect(printed(lexer, "1 > > 2") == "<error>",
         "spaced angle tokens should not become a shift operator");
  expect(printed(lexer, "value *= 2") == "(*= value 2)",
         "compound assignment should parse as a right-associative assignment");
  const std::string assignment = printed(lexer, "outer::value = 1.5");
  expect(assignment == "(= outer::value float32(0x3fc00000))",
         "assignment should print its qualified target and binary float: " +
             assignment);
}

void typeSpellings() {
  lang::Lexer lexer;
  expect(printed(lexer, "sizeof(int16_t[3][4])") == "(sizeof int16_t[3][4])",
         "sizeof(type) should parse as a dedicated type-only AST node");
  expect(printed(lexer, "alignof(const uint32_t*)") ==
             "(alignof const uint32_t*)",
         "alignof(type) should retain pointee const and raw-pointer syntax");
  expect(printed(lexer, "sizeof int32_t") == "<error>",
         "layout queries should require a parenthesized type operand");

  expect(printed(lexer, "[]() -> const native::Cell* { return nullptr; }") ==
             "(lambda [] -> const native::Cell*)",
         "AST printing should keep pointee const before the first qualified "
         "type segment");
  for (const std::string type :
       {"native::Cell***", "const native::Cell***", "native::Cell* const*",
        "const native::Cell* const** const*"})
    expect(printed(lexer, "[]() -> " + type + " { return nullptr; }") ==
               "(lambda [] -> " + type + ")",
           "AST printing should preserve every pointer layer and its own "
           "pointee qualifier: " +
               type);
  expect(printed(lexer, "[]() -> native::Cell* const { return nullptr; }") ==
             "<error>",
         "trailing const should not introduce C++ binding constness");
}

void initializerSpellings() {
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan("struct Box { int value; };\n"
                                 "int main() { Box boxed{1}; return 0; }"));
  const lang::Program program = parser.parse();
  const lang::DirectInitializer *initializer = nullptr;
  for (const auto &declaration : program.declarations()) {
    const auto *function =
        dynamic_cast<const lang::FunctionDecl *>(declaration.get());
    if (function == nullptr || function->body() == nullptr ||
        function->body()->statements().empty())
      continue;
    const auto *boxed = dynamic_cast<const lang::VariableDecl *>(
        function->body()->statements().front().get());
    if (boxed != nullptr)
      initializer = dynamic_cast<const lang::DirectInitializer *>(
          boxed->initializer().get());
  }
  expect(!parser.hadError() && initializer != nullptr &&
             lang::AstPrinter().print(*initializer) == "(direct-init 1)",
         "the AST should preserve direct constructor arguments");
}

} // namespace

GTI_UNIT_TEST(expression_syntax_api) {
  characterAndStringEscapes();
  operatorPrecedence();
  typeSpellings();
  initializerSpellings();
  return failures == 0 ? 0 : 1;
}
