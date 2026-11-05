#include "gti/ast_clone.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/frontend_cache.h"
#include "gti/frontend_work.h"
#include "gti/language_queries.h"
#include "gti/lexer.h"
#include "gti/mir_constant_evaluator.h"
#include "gti/mir_procedural.h"
#include "gti/mir_verifier.h"
#include "gti/parser.h"
#include "gti/procedural_syntax.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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
lang::FrontendResult
analyze(std::string source,
        std::string macro = std::string(proceduralDeclaration),
        lang::FrontendOptions options = {}) {
  const auto root = std::filesystem::current_path() / "proc-fixtures";
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend(std::move(options))
      .analyze(root / "main.gti", std::move(source), {library / "prelude.gti"},
               {{(root / "library.gti").string(), std::move(macro)}},
               {library});
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
      "inspect(std::syntax::tokens input) { return std::syntax::parse(input); "
      "} int main(){return 0;}";
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
      "import \"library.gti\"; int main() { return html!({ "
      "<h1>{unknown}</h1> }) - 42; }";
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
void sharedSyntaxConstruction() {
  std::uint32_t identity = 100;
  auto budget = std::make_shared<lang::SyntaxBudget>();
  std::size_t work = 0;
  lang::SyntaxArena arena(
      10, 20, [&] { return ++identity; }, {}, {}, {}, budget,
      [&](std::size_t amount) { work += amount; });
  const auto atom = arena.capture(capture("x"));
  const auto empty = arena.slice(atom, 0, 0);
  auto joined = atom;
  for (int i = 0; i < 1024; ++i) {
    const std::array parts{empty, joined};
    joined = arena.join(parts);
  }
  check(arena.size(joined) == 1 && budget->tokens == 1,
        "joins retain shared pieces rather than copied growing token tables");
  check(arena.text(joined) == "x" && budget->tokens == 2 && work < 10000,
        "materialization preserves spelling with bounded linear work");
  const auto before = budget->tokens;
  check(arena.text(joined) == "x" && budget->tokens == before,
        "repeated inspection reuses its retained flat table");
  auto doubling = atom;
  rejects(
      [&] {
        for (int i = 0; i < 32; ++i) {
          const std::array parts{doubling, doubling};
          doubling = arena.join(parts);
        }
      },
      "shared DAG cannot conceal exponentially large output");
  auto nothing = empty;
  for (int i = 0; i < 64; ++i) {
    const std::array parts{nothing, nothing};
    nothing = arena.join(parts);
  }
  const auto beforeEmpty = work;
  check(arena.text(nothing).empty() && work - beforeEmpty < 10,
        "an empty shared DAG cannot require exponential traversal");
}
void execution() {
  const auto libraryPath =
      std::filesystem::path(__FILE__).parent_path().parent_path() /
      "examples/procedural-macro-library.gti";
  std::ifstream file(libraryPath);
  std::ostringstream source;
  source << file.rdbuf();
  check(file.good() || file.eof(), "procedural library fixture is readable");
  const std::string library = source.str();
  const auto accepted = [&](std::string program, std::string macros = {}) {
    auto result = analyze(std::move(program),
                          macros.empty() ? library : std::move(macros));
    if (!result.canGenerateCode())
      for (const auto &d : result.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
    check(result.canGenerateCode(),
          "procedural program reaches verified ordinary MIR");
    return result;
  };
  accepted("import \"library.gti\"; int main(){auto value=quoted!({word}); "
           "return value == \"word\" ? 0 : 1;}",
           R"(
export module library;
import std.syntax;
constexpr std::string_view inspected(std::syntax::tokens input) {
  return std::syntax::text(input);
}
export #macro(proc) std::syntax::expr quoted(std::syntax::tokens input) {
  auto token = std::syntax::slice(input, uint64_t(1), uint64_t(2));
  return std::syntax::parse(std::syntax::string_literal(inspected(token),
      std::syntax::location(input, uint64_t(1), uint64_t(2))));
}
)");
  auto identity = accepted("import \"library.gti\"; int main(){ int value=21; "
                           "return identity!({value + value}) - 42; }");
  for (const auto &function : identity.mir.functionInstances()) {
    check(!function.proceduralFunction,
          "procedural body never reaches runtime MIR");
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        check(!lang::syntaxIntrinsic(instruction.intrinsic),
              "syntax intrinsic never reaches runtime MIR");
  }
  accepted("import \"library.gti\"; int main(){ int saved=9; return "
           "twice_once!({saved}) - 18; }");
  accepted("import \"library.gti\"; int main(){ return "
           "identity!({twice_once!({21})}) - 42; }");
  accepted("import \"library.gti\"; T relay<T>(T value){ return "
           "identity!({value}); } int main(){ return relay<int>(42)-42; }");
  diagnostic(
      analyze(
          "import \"library.gti\"; int main(){return identity!({missing});}",
          library),
      "Undefined name");
  diagnostic(
      analyze("import \"library.gti\"; int main(){return identity!({1;});}",
              library),
      "trailing");
  diagnostic(analyze("import \"library.gti\"; int main(){int x = "
                     "identity!({true}); return 0;}",
                     library),
             "type");
  accepted("import \"library.gti\"; int main(){return "
           "identity!({identity!({1})}) - 1;}");
  diagnostic(analyze("import \"library.gti\"; class Hidden { int value=7; }; "
                     "int main(){Hidden h; return identity!({h.value});}",
                     library),
             "private");
  diagnostic(
      analyze("import \"library.gti\"; int& bad(int& input){int local=7; "
              "return identity!({local});} int main(){return 0;}",
              library),
      "must derive from read-only parameter");
  const std::string effectful =
      "export module library; import std.syntax; mut int state=0; "
      "export #macro(proc) std::syntax::expr effect(std::syntax::tokens input) "
      "{"
      "state += 1; return std::syntax::parse(std::syntax::slice(input, "
      "uint64_t(1), std::syntax::size(input)-uint64_t(1))); }";
  diagnostic(analyze("import \"library.gti\"; int main(){return effect!({1});}",
                     effectful),
             "constant");

  const std::string cycle = R"gti(
export module library;
import std.syntax;
export #macro(proc) std::syntax::expr cycle(std::syntax::tokens input) {
  auto origin = std::syntax::location(input, uint64_t(0), uint64_t(1));
  auto name = std::syntax::identifier("cycle", origin);
  mut auto output = std::syntax::join(name, std::syntax::punctuation("!", origin));
  output = std::syntax::join(output, std::syntax::punctuation("(", origin));
  output = std::syntax::join(output, input);
  output = std::syntax::join(output, std::syntax::punctuation(")", origin));
  return std::syntax::parse(output);
})gti";
  diagnostic(
      analyze("import \"library.gti\"; int main(){return cycle!({1});}", cycle),
      "recursive procedural");
  const std::string targetMacro = R"gti(
export module library;
import std.syntax;
export #macro(proc) std::syntax::expr target_value(std::syntax::tokens input) {
  auto origin = std::syntax::location(input, uint64_t(0), uint64_t(1));
#if target.arch == "arm64"
  return std::syntax::parse(std::syntax::literal("42", origin));
#else
  return std::syntax::parse(std::syntax::literal("7", origin));
#endif
})gti";
  const std::string targetConsumer =
      "import \"library.gti\"; int main(){return target_value!({});}";
  auto targetCache = std::make_shared<lang::FrontendCache>();
  for (const auto &[triple, value] :
       std::array{std::pair{"arm64-apple-macos", "42"},
                  std::pair{"x86_64-unknown-linux-gnu", "7"}}) {
    lang::FrontendOptions options;
    options.target = *lang::parseTargetTriple(triple);
    options.cache = targetCache;
    auto result = analyze(targetConsumer, targetMacro, options);
    check(result.canGenerateCode(),
          "procedural target selection is independent of host");
    auto view = lang::LanguageQueries().macroExpansion(
        result, result.sourceGraph.entryUnit(),
        targetConsumer.find("target_value!") + 2);
    check(view && view->text.find(value) != std::string::npos,
          "retained procedural inspection reflects selected target");
  }
  const std::string viewConsumer =
      "import \"library.gti\"; int main(){return twice_once!({21});}";
  auto viewResult = accepted(viewConsumer);
  auto view = lang::LanguageQueries().macroExpansion(
      viewResult, viewResult.sourceGraph.entryUnit(),
      viewConsumer.find("twice_once!") + 2);
  if (!view || view->text.find("__macro_local_") == std::string::npos ||
      view->text.find("omitted") != std::string::npos)
    std::cerr << "Inspection: " << (view ? view->text : "<no result>") << '\n';
  check(view && view->text.find("__macro_local_") != std::string::npos &&
            view->text.find("21") != std::string::npos &&
            view->text.find("omitted") == std::string::npos,
        "inspection renders generated lambda and hygienic parameter labels");
  for (const auto member :
       {&lang::CompileLimits::syntaxBytes, &lang::CompileLimits::syntaxTokens,
        &lang::CompileLimits::syntaxDepth}) {
    lang::FrontendOptions options;
    options.limits.*member = 1;
    auto result =
        analyze("import \"library.gti\"; int main(){return identity!({42});}",
                library, options);
    diagnostic(result, "limit exceeded");
    check(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto &d) { return !d.hints.empty(); }),
          "syntax resource diagnostics include configuration guidance");
  }
  auto ordinary = analyze("constexpr int same(int value){return value;} "
                          "int main(){return same(1);}",
                          "export module library;");
  check(ordinary.canGenerateCode(), "ordinary constant profile fixture lowers");
  for (const auto &function : ordinary.hir.functionInstances()) {
    if (!function.source || function.source->name().lexeme != "same")
      continue;
    const auto forged = lang::makeConstantAggregate(
        {.kind = lang::ConstantAggregateKind::TransientSyntax, .identity = 1});
    const std::array<lang::ConstantValue, 1> direct{forged};
    const auto rejected =
        lang::evaluateMirConstantFunction(ordinary.mir, function.id, direct);
    check(!rejected && rejected.failure ==
                           lang::ConstantEvaluationFailure::InvalidOperands,
          "ordinary constant requests reject forged syntax handles");
    const std::array<lang::ConstantValue, 1> nested{lang::makeConstantAggregate(
        {.kind = lang::ConstantAggregateKind::Object,
         .fields = {{1, forged}}})};
    check(!lang::evaluateMirConstantFunction(ordinary.mir, function.id, nested),
          "aggregate nesting cannot publish a syntax handle");
  }

  const std::string reusedSource = R"gti(
import "library.gti";
#macro int double_value(int value) { return value + value; }
int main(){ return double_value(identity!({21})) - 42; }
)gti";
  for (bool reuse : {true, false}) {
    lang::FrontendWorkMetrics metrics;
    lang::FrontendOptions options;
    options.reuseConstantEvaluations = reuse;
    options.workMetrics = &metrics;
    auto result = analyze(reusedSource, library, options);
    if (!result.canGenerateCode())
      for (const auto &d : result.diagnostics)
        std::cerr << "Reuse: " << d.code << ": " << d.message << '\n';
    check(result.canGenerateCode(),
          "typed/procedural nesting works with reuse on and off");
    check(metrics.proceduralExecutions > 0,
          "reuse controls do not bypass procedural execution admission");
  }
  auto constant = accepted(
      "import \"library.gti\"; constexpr int answer = identity!({42}); "
      "static_assert(answer == 42); int main(){return answer-42;}");
  (void)constant;
  {
    lang::FrontendWorkMetrics metrics;
    lang::FrontendOptions options;
    options.workMetrics = &metrics;
    auto repeated = analyze("import \"library.gti\"; int take(int "
                            "value=identity!({42})){return value;} "
                            "int main(){return take()+take()-84;}",
                            library, options);
    check(repeated.canGenerateCode(),
          "procedural default arguments preserve caller checking");
    if (!repeated.canGenerateCode())
      for (const auto &d : repeated.diagnostics)
        std::cerr << "Default argument: " << d.code << ": " << d.message
                  << '\n';
    check(metrics.proceduralExecutions == 1,
          "one declaration-owned expansion supplies both default evaluations");
  }
  const std::string inspectionLibrary = R"gti(
export module library;
import std.syntax;
export #macro(proc) std::syntax::expr inspect(std::syntax::tokens input){
  auto whole=std::syntax::group(input,uint64_t(0));
  auto origin=std::syntax::location(whole,uint64_t(0),uint64_t(1));
  if(std::syntax::text(whole)!="{42}" ||
     std::syntax::kind(whole,uint64_t(0))==std::syntax::kind(whole,uint64_t(1)) ||
     !std::syntax::adjacent(whole,uint64_t(0))) {
    return std::syntax::fail_related(origin,"unexpected input",origin);
  }
  return std::syntax::parse(std::syntax::slice(whole,uint64_t(1),uint64_t(2)));
}
export #macro(proc) std::syntax::expr unbound(std::syntax::tokens input){
  auto origin=std::syntax::location(input,uint64_t(0),uint64_t(1));
  return std::syntax::parse(std::syntax::reference(std::syntax::fresh("outside",origin)));
}
export constexpr int outside=42;
)gti";
  auto inspected =
      analyze("import \"library.gti\"; int main(){return inspect!({42})-42;}",
              inspectionLibrary);
  check(inspected.canGenerateCode(),
        "source token inspection executes through the syntax API");
  diagnostic(
      analyze("import \"library.gti\"; int main(){return inspect!({ 42});}",
              inspectionLibrary),
      "unexpected input");
  diagnostic(analyze("import \"library.gti\"; int main(){return unbound!({});}",
                     inspectionLibrary),
             "Undefined name");
  std::string freshLibrary = R"gti(
export module library;
import std.syntax;
export #macro(proc) std::syntax::expr fresh_pair(std::syntax::tokens input){
  auto origin=std::syntax::location(input,uint64_t(0),uint64_t(1));
  auto first=std::syntax::reference(std::syntax::fresh("value",origin));
  auto second=std::syntax::reference(std::syntax::fresh("value",origin));
  mut auto result=std::syntax::punctuation("[",origin);
)gti";
  const auto append = [&](std::string part) {
    freshLibrary += "result=std::syntax::join(result," + part + ");\n";
  };
  const auto punctuation = [&](std::string text) {
    append("std::syntax::punctuation(\"" + text + "\",origin)");
  };
  const auto keyword = [&](std::string text) {
    append("std::syntax::keyword(\"" + text + "\",origin)");
  };
  punctuation("]");
  punctuation("(");
  keyword("int");
  append("first");
  punctuation(",");
  keyword("int");
  append("second");
  punctuation(")");
  punctuation("->");
  keyword("int");
  punctuation("{");
  keyword("return");
  append("first");
  punctuation("+");
  append("second");
  punctuation(";");
  punctuation("}");
  punctuation("(");
  append("std::syntax::literal(\"1\",origin)");
  punctuation(",");
  append("std::syntax::literal(\"2\",origin)");
  punctuation(")");
  freshLibrary += "return std::syntax::parse(result);}";
  auto distinct =
      accepted("import \"library.gti\"; int main(){return fresh_pair!({})-3;}",
               freshLibrary);
  bool distinctParameters = false;
  for (const auto &lambda : distinct.mir.lambdaInstances())
    if (lambda.parameterBindings.size() == 2)
      distinctParameters |=
          lambda.parameterBindings[0] != lambda.parameterBindings[1];
  check(distinctParameters,
        "same-spelled fresh parameters retain distinct MIR identities");
  auto limited = lang::FrontendOptions{};
  limited.limits.syntaxNodes = 1;
  diagnostic(
      analyze("import \"library.gti\"; int main(){return identity!({1});}",
              library, limited),
      "syntax-nodes");
  limited = {};
  limited.limits.constexprSteps = 8;
  diagnostic(
      analyze("import \"library.gti\"; int main(){return identity!({1});}",
              library, limited),
      "constexpr-steps");
  const std::string rejected =
      "export module library; import std.syntax; "
      "export #macro(proc) std::syntax::expr reject(std::syntax::tokens input) "
      "{ "
      "return "
      "std::syntax::fail(std::syntax::location(input,uint64_t(1),uint64_t(2)), "
      "\"library rejection\"); }";
  auto failure = analyze(
      "import \"library.gti\"; int main(){return reject!({42});}", rejected);
  diagnostic(failure, "library rejection");
  const auto found = std::find_if(
      failure.diagnostics.begin(), failure.diagnostics.end(),
      [](const auto &d) {
        return d.message.find("library rejection") != std::string::npos;
      });
  check(found != failure.diagnostics.end() && found->related.size() >= 2,
        "library failure retains definition and invocation traces");
}

void generatedDeclarationContext() {
  std::string library = R"gti(
export module library;
import std.syntax;
namespace library_detail {
struct State { int answer; State() : answer(42) {} };
export #macro(proc) std::syntax::expr state(std::syntax::tokens input) {
  auto origin = std::syntax::location(input, uint64_t(0), uint64_t(1));
  auto binding = std::syntax::reference(std::syntax::fresh("saved", origin));
  mut auto result = std::syntax::punctuation("[", origin);
)gti";
  for (const auto &[kind, spelling] :
       std::vector<std::pair<std::string, std::string>>{
           {"punctuation", "]"},
           {"punctuation", "("},
           {"punctuation", ")"},
           {"punctuation", "->"},
           {"keyword", "int"},
           {"punctuation", "{"},
           {"identifier", "State"},
           {"binding", ""},
           {"punctuation", "{"},
           {"punctuation", "}"},
           {"punctuation", ";"},
           {"keyword", "return"},
           {"binding", ""},
           {"punctuation", "."},
           {"identifier", "answer"},
           {"punctuation", ";"},
           {"punctuation", "}"},
           {"punctuation", "("},
           {"punctuation", ")"}}) {
    library += "result=std::syntax::join(result,";
    library += kind == "binding"
                   ? "binding"
                   : "std::syntax::" + kind + "(\"" + spelling + "\",origin)";
    library += ");\n";
  }
  library += "return std::syntax::parse(result); } }";
  auto result =
      analyze("import \"library.gti\"; struct State {std::string_view "
              "answer=\"caller\";}; "
              "int main(){int saved=7;return library_detail::state!({})-42;}",
              library);
  if (!result.canGenerateCode())
    for (const auto &d : result.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
  check(result.canGenerateCode(), "generated local type uses definition "
                                  "imports and namespace, not caller span");
  diagnostic(analyze("import \"library.gti\"; int main(){"
                     "library_detail::State hidden{};return 0;}",
                     library),
             "private");
}

void snapshotIdentity() {
  const std::string library = R"(
export module library;
import std.syntax;
export #macro(proc) std::syntax::expr selected(std::syntax::tokens input) {
  auto origin = std::syntax::location(input, uint64_t(0), uint64_t(1));
#ifdef SELECT
  return std::syntax::parse(std::syntax::literal("17", origin));
#else
  return std::syntax::parse(std::syntax::literal("23", origin));
#endif
}
)";
  const std::string consumer =
      "import \"library.gti\"; int main(){return selected!({});}";
  auto cache = std::make_shared<lang::FrontendCache>();
  for (bool selected : {false, true, false}) {
    std::string expected;
    for (bool reuse : {false, true}) {
      lang::FrontendOptions options;
      options.cache = cache;
      options.reuseConstantEvaluations = reuse;
      if (selected)
        options.configurationFlags = {"SELECT"};
      auto result = analyze(consumer, library, options);
      check(result.canGenerateCode(),
            "cached source respects procedural configuration");
      auto view = lang::LanguageQueries().macroExpansion(
          result, result.sourceGraph.entryUnit(),
          consumer.find("selected!") + 2);
      check(view &&
                view->text.find(selected ? "17" : "23") != std::string::npos,
            "configuration changes cannot reuse a stale expansion");
      if (!reuse && view)
        expected = view->text;
      else
        check(view && view->text == expected,
              "cold/warm expansion is identical");
      options.limits.syntaxTokens = 1;
      diagnostic(analyze(consumer, library, options), "syntax-tokens");
    }
  }
}
void executionCancellation() {
  // Build a verified body independently of frontend cancellation, then stop
  // the procedural profile after execution has begun. The patched descriptors
  // model the trusted identities normally supplied by semantic staging.
  auto frontend = analyze(R"gti(
struct Tokens {}; struct Expression { Expression(){} }; struct Span {}; struct Binding {};
int anchor(Tokens t, Expression e, Span s, Binding b){return 0;}
Expression spinning(Tokens input){mut bool running=true; while(running){} return Expression();}
int main(){return 0;}
)gti",
                          "export module library;");
  check(frontend.canGenerateCode(),
        "cancellation fixture lowers to verified MIR");
  if (!frontend.canGenerateCode()) {
    for (const auto &d : frontend.diagnostics)
      std::cerr << "Cancellation fixture: " << d.code << ": " << d.message
                << '\n';
    return;
  }
  std::array<lang::SemanticType, 4> types;
  lang::HirFunctionInstanceId root = 0;
  for (const auto &function : frontend.hir.functionInstances()) {
    if (function.source && function.source->name().lexeme == "anchor")
      std::copy_n(function.parameterTypes.begin(), 4, types.begin());
    if (function.source && function.source->name().lexeme == "spinning")
      root = function.id;
  }
  auto program = frontend.mir;
  for (auto &record : const_cast<std::vector<lang::MirClassInstance> &>(
           program.classInstances()))
    for (std::size_t i = 0; i < types.size(); ++i)
      if (record.type == types[i])
        record.syntaxKind = static_cast<lang::CompileTimeSyntaxKind>(i + 1);
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           program.functionInstances()))
    if (function.id == root) {
      function.proceduralFunction = true;
      function.constexprFunction = true;
    }
  check(root && lang::verifyMirProgram(program).valid(),
        "procedural cancellation starts with verified MIR");
  std::uint32_t next = 20;
  lang::SyntaxArena arena(10, 11, [&] { return ++next; });
  std::size_t polls = 0;
  lang::MirProceduralRequest request{.arena = arena,
                                     .input = arena.capture(capture("{}")),
                                     .types = types,
                                     .cancelled = [&] { return ++polls >= 8; }};
  const auto result =
      lang::evaluateMirProceduralFunction(program, root, request);
  if (result.execution.detail.find("cancelled") == std::string::npos)
    std::cerr << "Cancellation: " << result.execution.detail << '\n';
  check(polls == 8 && !result.execution && !result.expression &&
            !result.execution.value &&
            result.execution.detail.find("cancelled") != std::string::npos,
        "in-flight cancellation publishes neither syntax nor constants");
}
} // namespace
int main() {
  parserAndFormatter();
  arena();
  sharedSyntaxConstruction();
  semantics();
  execution();
  executionCancellation();
  generatedDeclarationContext();
  snapshotIdentity();
  return failures ? 1 : 0;
}
