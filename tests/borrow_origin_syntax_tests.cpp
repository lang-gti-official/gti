#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"
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

void testAttributes() {
  const std::string source = R"(
[[track_caller]] int&<a> borrow<lifetime a>(int&<a> owner) { return owner; }
struct Holder {
  int value;
  int&<this> get() { return this.value; }
};
[[noreturn, track_caller]] void stop() { while (true) {} }
[[c_abi]] struct Native { int value; };
)";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "borrow-origin-syntax.gti"));
  const auto program = parser.parse();
  expect(!lexer.hadError() && !parser.hadError(),
         "lifetime contracts and existing attribute families must parse");
  expect(program.declarations().size() == 4, "declarations must be preserved");
  const auto *function =
      lang::stmtAs<lang::FunctionDecl>(program.declarations()[0].get());
  expect(function && function->hasFunctionAttribute("track_caller") &&
             !function->findFunctionAttribute("missing") &&
             function->returnType().referenceLifetime &&
             function->returnType().referenceLifetime->name.lexeme == "a",
         "a result region and attribute queries must coexist");
}

// RFC 0004 replaced the attribute with lifetime syntax; a written attribute
// is a parse error that names the replacement.
void testMalformedAttributes() {
  struct Fixture {
    const char *attribute;
    const char *message;
  };
  for (const Fixture fixture : {
           Fixture{"[[borrowed_from(owner)]]",
                   "'[[borrowed_from]]' was removed"},
           Fixture{"[[borrowed_from(this)]]",
                   "'[[borrowed_from]]' was removed"},
           Fixture{"[[track_caller, borrowed_from(owner)]]",
                   "'[[borrowed_from]]' was removed"},
           Fixture{"[[noreturn(owner)]]", "Unknown function attribute"},
           Fixture{"[[unknown(owner)]]", "Unknown function attribute"},
           Fixture{"[[track_caller, unknown(owner)]]",
                   "Unknown function attribute"},
       }) {
    lang::Lexer lexer;
    lang::Parser parser(
        lexer.scan(std::string(fixture.attribute) +
                   " int& borrow(int& owner) { return owner; }"));
    [[maybe_unused]] const auto program = parser.parse();
    expect(parser.hadError() && !parser.errors().empty(),
           "removed or malformed attributes must not be accepted");
    const auto &diagnostic = parser.errors().front();
    if (diagnostic.message.find(fixture.message) == std::string::npos)
      std::cerr << fixture.attribute << ": " << diagnostic.message << '\n';
    expect(diagnostic.code == "GTI-P0001" &&
               diagnostic.phase == lang::DiagnosticPhase::Parsing &&
               diagnostic.message.find(fixture.message) != std::string::npos,
           "attribute must have its owning parser diagnostic");
  }
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(
      "concept readable<C> = requires(C& owner) { { owner.read() } -> int& "
      "[[borrowed_from(owner)]]; };"));
  [[maybe_unused]] const auto program = parser.parse();
  expect(parser.hadError() && parser.errors().front().message.find(
                                  "names its supplier with a "
                                  "region") != std::string::npos,
         "a requirement result names its supplier with a region");
}

void testFormatting() {
  const std::string source = "[[track_caller]] int&<a> borrow<lifetime a>("
                             "int&<a> owner){return owner;}\n"
                             "struct Holder{int value;"
                             "int&<this> get(){return this.value;}};";
  const lang::Formatter formatter;
  const auto formatted = formatter.format(source);
  expect(formatted.find("int&<a> borrow<lifetime a>(int&<a> owner)") !=
                 std::string::npos &&
             formatted.find("int&<this> get()") != std::string::npos,
         "formatter must preserve lifetime contracts");
  expect(formatter.format(formatted) == formatted,
         "lifetime contract formatting must be idempotent");
  lang::Lexer beforeLexer, afterLexer;
  const auto before = beforeLexer.scan(source);
  const auto after = afterLexer.scan(formatted);
  expect(before.size() == after.size(), "formatter must preserve token count");
  for (std::size_t i = 0; i < before.size(); ++i) {
    if (before[i].kind == lang::TokenKind::END_OF_FILE &&
        after[i].kind == lang::TokenKind::END_OF_FILE)
      continue;
    if (before[i].kind != after[i].kind || before[i].lexeme != after[i].lexeme)
      std::cerr << "Token " << i << ": '" << before[i].lexeme << "' -> '"
                << after[i].lexeme << "'\nFormatted:\n"
                << formatted;
    expect(before[i].kind == after[i].kind &&
               before[i].lexeme == after[i].lexeme,
           "formatter must preserve every contract token");
  }
  lang::Parser parser(after);
  [[maybe_unused]] const auto program = parser.parse();
  expect(!parser.hadError(), "formatted contracts must remain parseable");
}

void testResolvedSignatures() {
  const std::string source = R"(
int&<r> choose<lifetime r>(int& left, int&<r> right) { return right; }
T&<a> relay<T, lifetime a>(T&<a> owner) { return owner; }
struct Holder {
  int value = 7;
  int&<this> get() { return this.value; }
public:
  Holder() = default;
};
int& implicit(int& owner) { return owner; }
int main() {
  int left = 3;
  int right = 4;
  Holder holder{};
  return choose(left, right) + holder.get() + relay(right) + implicit(left);
}
)";
  const auto frontend =
      lang::Frontend().analyze("borrow-origin-hover.gti", source);
  expect(frontend.canGenerateCode(),
         "lifetime contract hover fixture must analyze");
  const lang::LanguageQueries queries;
  const auto unit = frontend.sourceGraph.entryUnit();
  const auto check = [&](std::size_t offset, std::string_view contract) {
    const auto hover = queries.hover(frontend, unit, offset + 1);
    if (!hover || hover->signature.find(contract) == std::string::npos ||
        hover->signature.find("borrowed_from") != std::string::npos) {
      std::cerr << "Expected contract: " << contract
                << "\nActual: " << (hover ? hover->signature : "<missing>")
                << '\n';
      expect(false, "hover must render the written result region");
    }
  };
  check(source.find("choose<"), "int32_t&<r> right)");
  check(source.rfind("choose("), "int32_t&<r> right)");
  check(source.find("get()"), "&<this>");
  check(source.rfind("get()"), "&<this>");
  check(source.find("relay<T"), "&<a>");
  const auto implicit =
      queries.hover(frontend, unit, source.find("implicit(") + 1);
  expect(implicit &&
             implicit->signature.find("borrowed_from") == std::string::npos &&
             implicit->signature.find("&<") == std::string::npos,
         "inferred origins must not invent an explicit source contract");
}
} // namespace

int main() {
  {
    const std::string source =
        "int main() { auto relay = [](mut int& value) -> mut int& { "
        "return value; }; return 0; }";
    lang::Lexer lexer;
    lang::Parser parser(lexer.scan(source, "lambda-reference-syntax.gti"));
    const auto program = parser.parse();
    expect(!parser.hadError() && program.declarations().size() == 1,
           "mutable lambda result syntax must parse");
    const auto *function =
        lang::stmtAs<lang::FunctionDecl>(program.declarations().front().get());
    const auto *binding =
        function && function->body()
            ? lang::stmtAs<lang::VariableDecl>(
                  function->body()->statements().front().get())
            : nullptr;
    const auto *lambda =
        binding ? lang::exprAs<lang::Lambda>(binding->initializer().get())
                : nullptr;
    expect(lambda && lambda->returnType().reference &&
               lambda->returnType().referenceMutability,
           "lambda result must preserve its reference and access tokens");
    const auto &access = *lambda->returnType().referenceMutability;
    expect(access.source == "lambda-reference-syntax.gti" &&
               access.position == source.find("-> mut") + 3 &&
               access.lexeme == "mut",
           "lambda result access must retain its exact source span");
  }
  testAttributes();
  testMalformedAttributes();
  testFormatting();
  testResolvedSignatures();
}
