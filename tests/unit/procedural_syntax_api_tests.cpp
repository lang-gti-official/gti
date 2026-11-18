// Procedural-macro syntax API checks with no source-level observable: the
// parser's opaque capture of macro input (exact spelling, trivia, clone
// expansion context, malformed delimiters) and the formatter's preservation
// of it; and the SyntaxArena's trivia retention, escaped string-literal
// construction, construction contexts, fresh-binding identities, sealing,
// stale/foreign handle and span rejection, directive rejection, node/depth
// budgets and cancellation. (Restored unchanged from the former
// procedural_syntax_tests.cpp, whose GTI sources became tests/ui/macros
// cases.)
#include "gti/ast_clone.h"
#include "gti/formatter.h"
#include "gti/lexer.h"
#include "gti/parser.h"
#include "gti/procedural_syntax.h"
#include "unit_test.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void check(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
template <typename F> void rejects(F operation, const char *message) {
  try {
    operation();
    check(false, message);
  } catch (const lang::SyntaxError &) {
  }
}
lang::SyntaxCapture capture(std::string source) {
  lang::Lexer lexer;
  auto tokens = lexer.scan(source, "caller.gti");
  check(!lexer.hadError(), "syntax fixture lexes");
  tokens.pop_back();
  return lang::captureSyntax(tokens, source);
}
void parserAndFormatter() {
  const std::string source =
      "web::html!({ <h1 a=\"b\"> /* trivia */ {name[0]} </h1> })";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "input.gti"), source);
  auto expression = parser.parseExpression();
  const auto *call = lang::exprAs<lang::Call>(expression.get());
  check(call && call->capturedArguments() && call->arguments().empty(),
        "explicit macro captures opaque tokens");
  if (call && call->capturedArguments()) {
    const auto &input = *call->capturedArguments();
    check(input.text == source.substr(source.find('(')),
          "capture keeps exact spelling, punctuation and trivia");
    auto copy = lang::cloneExpr(*call, 71);
    const auto *cloned = lang::exprAs<lang::Call>(copy.get());
    check(cloned && cloned->capturedArguments()->tokens[1].expansion == 71,
          "cloned input retains expansion context");
  }
  lang::Parser ordinary(lexer.scan("f({1, 2})"));
  const auto normal = ordinary.parseExpression();
  const auto *normalCall = lang::exprAs<lang::Call>(normal.get());
  check(normalCall && !normalCall->capturedArguments() &&
            normalCall->arguments().size() == 1,
        "ordinary braced calls retain their original AST and grammar");
  lang::Parser declaration(lexer.scan(
      "#macro(proc) std::syntax::expr html(std::syntax::tokens input) {}"));
  const auto program = declaration.parse();
  check(!declaration.hadError() && program.declarations().size() == 1 &&
            lang::stmtAs<lang::FunctionDecl>(program.declarations()[0].get())
                ->isProceduralMacro(),
        "declaration identity distinguishes procedural from typed macros");
  for (const auto malformed :
       {"html!({ [ })", "html!({ <h1> }) extra", "html!({"}) {
    lang::Parser invalid(lexer.scan(malformed));
    check(!invalid.parseExpression() && invalid.hadError(),
          "malformed delimiters/trailing input rejected");
  }
  const lang::Formatter formatter;
  const auto formatted =
      formatter.format("int main(){auto x=" + source + ";return 0;}");
  check(formatted.find("{ <h1 a=\"b\"> /* trivia */ {name[0]} </h1> }") !=
            std::string::npos,
        "formatter preserves opaque macro input");
  check(formatter.format(formatted) == formatted,
        "opaque formatting is idempotent");
}
void arena() {
  std::uint32_t identity = 100;
  lang::SealedSyntaxExpression retained;
  {
    lang::SyntaxArena arena(10, 20, [&] { return ++identity; }, {}, {},
                            {{{"macro.gti", 1, 5, 1}, "Macro definition."}});
    const auto input = arena.capture(capture("caller /*comment*/ + 2"));
    check(arena.text(input) == "caller /*comment*/ + 2",
          "arena retains trivia");
    const auto grouped = arena.capture(capture("{a([b]) /*gap*/ c}"));
    check(arena.text(arena.group(grouped, 0)) == "{a([b]) /*gap*/ c}",
          "nested group inspection preserves original trivia");
    check(arena.adjacent(grouped, 0) && !arena.adjacent(input, 0),
          "punctuation adjacency is independent of diagnostic spans");
    const auto origin = arena.span(input, 0, 1);
    const std::string quoted =
        "quotes\" backslash\\ newline\n nul" + std::string(1, '\0');
    const auto quotedSyntax = arena.stringLiteral(quoted, origin);
    check(arena.size(quotedSyntax) == 1 && arena.token(quotedSyntax, 0).kind ==
                                               lang::TokenKind::STRING_LITERAL,
          "text construction produces exactly one escaped string literal");
    check(std::get<std::string>(arena.token(quotedSyntax, 0).literal) == quoted,
          "quoted text round trips embedded NUL, escapes and newlines");
    (void)arena.parse(quotedSyntax);
    check(arena.token(arena.punctuation("::", origin), 0).kind ==
              lang::TokenKind::SCOPE,
          "qualified construction uses an ordinary scope token");
    const auto injection =
        arena.stringLiteral("\"; unsafe {evil();} //", origin);
    check(arena.size(injection) == 1, "text contents cannot inject GTI syntax");
    const auto name = arena.identifier("helper", origin);
    check(arena.token(name, 0).expansion == 10,
          "constructed identifier uses definition context");
    check(arena.token(input, 0).expansion == 20, "input uses caller context");
    const auto first = arena.freshBinding("local", origin);
    const auto second = arena.freshBinding("local", origin);
    const auto firstRef = arena.reference(first);
    const auto secondRef = arena.reference(second);
    check(arena.token(firstRef, 0).expansion !=
              arena.token(secondRef, 0).expansion,
          "same-spelled fresh bindings have distinct identities");
    check(arena.token(firstRef, 0).expansion ==
              arena.token(arena.reference(first), 0).expansion,
          "references retain their binding identity");
    const auto caller = arena.parse(input);
    const auto spliced = arena.splice(caller);
    check(arena.token(spliced, 0).expansion == 20,
          "splicing does not acquire definition access");
    const std::array parts{arena.literal("1", origin),
                           arena.punctuation("+", origin), spliced};
    retained = arena.seal(arena.parse(arena.join(parts)));
    const auto location = arena.span(
        arena.identifier("a_very_long_generated_name", origin), 0, 1);
    check(arena.failure(location, "message").primary.end == 6,
          "generated spelling cannot extend its validated source span");
    lang::SyntaxArena other(30, 40, [&] { return ++identity; });
    rejects([&] { (void)other.size(input); }, "foreign arena handle rejected");
    rejects([&] { (void)arena.identifier("unsafe", origin); },
            "builders cannot manufacture unsafe authority");
    rejects([&] { (void)arena.span(input, 0, 99); },
            "forged span range rejected");
    rejects([&] { (void)arena.parse(arena.capture(capture("x;"))); },
            "expression output forbids trailing semicolon");
    rejects([&] { (void)arena.capture(capture("#define EVIL")); },
            "captured directives rejected");
    arena.reset();
    rejects([&] { (void)arena.size(input); },
            "reset invalidates old generation");
    rejects([&] { (void)arena.failure(origin, "stale"); },
            "stale diagnostic span rejected");
  }
  check(retained.expression && retained.syntax &&
            retained.syntax->tokens.size() == 5,
        "sealed expression and provenance survive arena teardown");
  check(retained.expansionTrace.size() == 1,
        "sealed output retains expansion trace");
  lang::SyntaxArena limited(10, 20, [&] { return ++identity; }, {.nodes = 1});
  (void)limited.capture(capture("1"));
  rejects([&] { (void)limited.capture(capture("2")); },
          "discarded nodes still consume budget");
  lang::SyntaxArena shallow(10, 20, [&] { return ++identity; }, {.depth = 2});
  const auto opening = shallow.capture(capture("("));
  const lang::SyntaxTokens nested[] = {opening, opening, opening};
  rejects([&] { (void)shallow.join(nested); },
          "joining shallow fragments cannot bypass the nesting limit");
  (void)shallow.parse(shallow.capture(capture("((1))")));
  rejects([&] { (void)shallow.capture(capture("(((1)))")); },
          "captured nesting uses the same output limit");
  lang::SyntaxArena cancelled(
      10, 20, [&] { return ++identity; }, {}, [] { return true; });
  rejects([&] { (void)cancelled.capture(capture("1")); },
          "cancellation publishes no partial syntax");
}
} // namespace

GTI_UNIT_TEST(procedural_macro_parsing) {
  parserAndFormatter();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(procedural_syntax_arena) {
  arena();
  return failures == 0 ? 0 : 1;
}
