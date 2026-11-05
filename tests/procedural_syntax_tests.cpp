#include "gti/ast_clone.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/lexer.h"
#include "gti/parser.h"
#include "gti/procedural_syntax.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
int failures = 0;
constexpr std::string_view proceduralDeclaration =
    "export module library; import std.syntax; export #macro(proc) "
    "std::syntax::expr html(std::syntax::tokens input) { "
    "return std::syntax::parse(std::syntax::literal(\"42\", "
    "std::syntax::location(input, uint64_t(0), uint64_t(1)))); }";
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
lang::FrontendResult
analyze(std::string source,
        std::string macro = std::string(proceduralDeclaration)) {
  const auto root = std::filesystem::current_path() / "proc-fixtures";
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze(
      root / "main.gti", std::move(source), {library / "prelude.gti"},
      {{(root / "library.gti").string(), std::move(macro)}}, {library});
}
void diagnostic(const lang::FrontendResult &result, std::string_view message) {
  const bool found = std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(), [&](const auto &d) {
        return d.message.find(message) != std::string::npos;
      });
  check(!result.canGenerateCode() && found,
        "expected procedural diagnostic before code generation");
  if (!found)
    for (const auto &d : result.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
}
const lang::Diagnostic *locatedDiagnostic(const lang::FrontendResult &result,
                                          std::string_view code,
                                          lang::DiagnosticPhase phase,
                                          std::size_t begin, std::size_t end,
                                          int line = 1) {
  const auto found =
      std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
                   [&](const auto &d) { return d.code == code; });
  check(found != result.diagnostics.end(), "expected stable diagnostic code");
  if (found == result.diagnostics.end())
    return nullptr;
  const bool matches =
      found->phase == phase &&
      found->severity == lang::DiagnosticSeverity::Error &&
      found->primary.source ==
          (std::filesystem::current_path() / "proc-fixtures" / "main.gti")
              .string() &&
      found->primary.start == begin && found->primary.end == end &&
      found->primary.line == line;
  if (!matches)
    std::cerr << code << " at " << found->primary.source << ':'
              << found->primary.line << " bytes " << found->primary.start << '-'
              << found->primary.end << '\n';
  check(matches,
        "diagnostic retains exact source identity, byte span, phase and "
        "severity");
  check(found->fixes.empty() && found->hints.empty(),
        "syntax boundary diagnostics do not invent an automatic repair");
  return &*found;
}
void semantics() {
  auto unused = analyze("import \"library.gti\"; int main() { return 0; }");
  if (!unused.canGenerateCode())
    for (const auto &d : unused.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
  check(unused.canGenerateCode(), "unused procedural declaration needs no "
                                  "execution or runtime representation");
  const auto aliased =
      analyze("import std.syntax; using Input = std::syntax::tokens; "
              "using Output = std::syntax::expr; #macro(proc) Output "
              "chosen(Input input) { return std::syntax::parse(input); } int "
              "main(){return 0;}");
  if (!aliased.canGenerateCode())
    for (const auto &d : aliased.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
  check(aliased.canGenerateCode(),
        "signature checks trusted type identity through aliases");
  const auto memberAliased = analyze(
      "import std.syntax; struct Names { using Input = std::syntax::tokens; "
      "using Output = std::syntax::expr; }; #macro(proc) Names::Output "
      "chosen(Names::Input input) { return std::syntax::parse(input); } int "
      "main(){return 0;}");
  if (!memberAliased.canGenerateCode())
    for (const auto &d : memberAliased.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
  check(memberAliased.canGenerateCode(),
        "class-scoped aliases retain syntax capability identity");
  diagnostic(analyze("import std.syntax; using Input = std::syntax::tokens; "
                     "int main(){ Input value{}; return 0; }"),
             "compile-time-only");
  diagnostic(analyze("import std.syntax; #macro(proc) std::syntax::expr "
                     "invalid(mut std::syntax::tokens input) {} "
                     "int main(){return 0;}"),
             "immutable value parameters");
  const std::string hoverSource =
      "import std.syntax; #macro(proc) std::syntax::expr "
      "inspect(std::syntax::tokens input) {} int main(){return 0;}";
  const auto hoverSnapshot = analyze(hoverSource);
  if (!hoverSnapshot.canGenerateCode())
    for (const auto &d : hoverSnapshot.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
  const auto hover = lang::LanguageQueries().hover(
      hoverSnapshot, hoverSnapshot.sourceGraph.entryUnit(),
      hoverSource.find("inspect") + 2);
  check(hover && hover->signature.find("#macro(proc)") != std::string::npos,
        "shared LSP hover distinguishes procedural declarations");
  diagnostic(analyze("import std.syntax; #macro(proc) std::syntax::expr "
                     "same(std::syntax::tokens input) {} "
                     "int main(){ same!({x}); return 0;}"),
             "imported definition");
  diagnostic(analyze("import std.syntax; #macro(proc) std::syntax::expr "
                     "clash(std::syntax::tokens input) {} "
                     "int clash(int x){return x;} int main(){return 0;}"),
             "cannot have overloads");
  diagnostic(analyze("@compiler_type(\"syntax_tokens\") struct Counterfeit {}; "
                     "int main(){return 0;}"),
             "trusted standard-library unit");
  auto ordinary = analyze(
      "import std.array; int main(){ auto values = std::array<int, 2>({\n"
      "#define ORDINARY_FLAG\n1,2});\n#ifdef ORDINARY_FLAG\nreturn values[0] - "
      "1;\n#else\n#error \"flag lost\"\n#endif\n}");
  if (!ordinary.canGenerateCode())
    for (const auto &d : ordinary.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
  check(ordinary.canGenerateCode(),
        "ordinary braced calls keep configuration directive behavior");
  const std::string selectedSource =
      "import \"library.gti\"; int main() { html!({ "
      "<h1>{unknown}</h1> }); return 0; }";
  auto selected = analyze(selectedSource);
  check(selected.sourceValid && selected.syntaxValid,
        "opaque markup reaches semantic name selection");
  if (!selected.canGenerateCode())
    for (const auto &d : selected.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
  check(selected.canGenerateCode(),
        "source-defined procedural construction executes through MIR");
  check(std::none_of(selected.diagnostics.begin(), selected.diagnostics.end(),
                     [](const auto &d) {
                       return d.message.find("Unknown variable") !=
                                  std::string::npos ||
                              d.code == "GTI-B0001";
                     }),
        "no runtime probing or internal compiler failure for opaque input");
  diagnostic(
      analyze(
          "import \"library.gti\"; int main() { html!({a}, {b}); return 0; }"),
      "exactly one braced");
  diagnostic(
      analyze("import \"library.gti\"; int main() { html!(1); return 0; }"),
      "exactly one braced");
  diagnostic(
      analyze("import \"library.gti\"; int main() { html({1}); return 0; }"),
      "name!({...})");
  diagnostic(analyze("import \"library.gti\"; int main() { int html = 1; "
                     "html!({x}); return 0; }"),
             "name before '!'");
  diagnostic(
      analyze("int f(int x) {return x;} int main(){ f!({x}); return 0;}"),
      "name before '!'");
  diagnostic(analyze("import std.syntax; int main() { auto x = "
                     "std::syntax::tokens(); return 0; }"),
             "runtime values");
  diagnostic(analyze("import std.syntax; std::syntax::tokens value; int "
                     "main(){return 0;}"),
             "compile-time-only");
  diagnostic(analyze("import \"library.gti\"; int main(){return 0;}",
                     "export module library; import std.syntax; export "
                     "#macro(proc) int html(int x) {}"),
             "exactly one std::syntax::tokens");
  const std::string directiveSource =
      "html!({\n#define STOLEN\nimport \"missing.gti\";\n});";
  auto directives = analyze(directiveSource);
  diagnostic(directives, "must be outside procedural macro input");
  const auto define = directiveSource.find("#define");
  if (const auto *directiveDiagnostic = locatedDiagnostic(
          directives, "GTI-I0015", lang::DiagnosticPhase::SourceLoading, define,
          define + 7, 2))
    check(directiveDiagnostic->related.empty(),
          "directive rejection needs no unrelated source locations");
  check(!directives.sourceValid &&
            std::none_of(directives.sourceGraph.imports().begin(),
                         directives.sourceGraph.imports().end(),
                         [](const auto &edge) {
                           return edge.spelling == "missing.gti";
                         }),
        "rejected input creates no import edges");
  const std::string unbalancedSource =
      "html!({ ]\n#define STOLEN\nimport \"missing.gti\";\n});";
  auto unbalanced = analyze(unbalancedSource);
  diagnostic(unbalanced, "Unbalanced delimiters");
  const auto mismatch = unbalancedSource.find(']');
  (void)locatedDiagnostic(unbalanced, "GTI-I0016",
                          lang::DiagnosticPhase::SourceLoading, mismatch,
                          mismatch + 1);
  check(std::none_of(
            unbalanced.sourceGraph.imports().begin(),
            unbalanced.sourceGraph.imports().end(),
            [](const auto &edge) { return edge.spelling == "missing.gti"; }),
        "unbalanced input cannot expose following directives to the loader");
}
} // namespace
int main() {
  parserAndFormatter();
  arena();
  semantics();
  return failures ? 1 : 0;
}
