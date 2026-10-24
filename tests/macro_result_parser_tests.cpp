#include "gti/language_queries.h"
#include "gti/lexer.h"
#include "gti/parser.h"

#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {
int failures = 0;
void check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void signatures() {
  const std::string source = R"(
#macro statement(int value) { use(value); }
#macro int choose(bool enabled, int first, int second) {
  if (enabled) { return first; }
  return second;
}
[[track_caller]] #macro T generic<T>(bool enabled, T first, T second) {
  return enabled ? first : second;
}
#macro std::optional<T> maybe<T>(T value) { return value; }
int main() { return choose(true, 1, 2); }
)";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "macro-result-parser.gti"), source);
  const auto parsed = parser.parse();
  for (const auto &diagnostic : parser.errors())
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  check(!parser.hadError(),
        "implicit and explicit macro result signatures parse");
  check(parsed.declarations().size() == 5,
        "explicit result parsing preserves all following declarations");
  if (parsed.declarations().size() != 5)
    return;
  for (std::size_t index = 0; index != 4; ++index) {
    const auto *macro =
        lang::stmtAs<lang::FunctionDecl>(parsed.declarations()[index].get());
    check(macro && macro->isMacro(), "result signatures retain macro identity");
    if (!macro || !macro->isMacro())
      continue;
    const auto &result = macro->returnType().name.last();
    check(result.generated == (index == 0),
          "only the omitted void result has a generated source token");
    check(result.lexeme == (index == 0   ? "void"
                            : index == 1 ? "int"
                            : index == 2 ? "T"
                                         : "optional"),
          "result type and macro name remain separate parser facts");
  }
}

void recovery() {
  for (const auto &broken :
       {std::string("#macro int broken( { }\n"),
        std::string("#macro int broken(bool value { }\n")}) {
    const std::string source =
        broken + "#macro int recovered(int value) { return value; }\n"
                 "int main() { return 0; }\n";
    lang::Lexer lexer;
    lang::Parser parser(lexer.scan(source, "macro-result-recovery.gti"),
                        source);
    const auto parsed = parser.parse();
    check(parser.hadError(),
          "incomplete explicit macro signatures are diagnosed");
    bool retained = false;
    for (const auto &declaration : parsed.declarations()) {
      const auto *macro = lang::stmtAs<lang::FunctionDecl>(declaration.get());
      retained |=
          macro && macro->isMacro() && macro->name().lexeme == "recovered";
    }
    check(retained, "explicit macro signature recovery retains the next macro");
  }
}

void signatureQueries() {
  const std::string source = R"(
#macro implicit_void() {}
#macro void explicit_void() { return; }
#macro int result(int value) { return value; }
#macro T generic<T>(T value) { return value; }
int main() { return 0; }
)";
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Semantics;
  const auto snapshot =
      lang::Frontend(options).analyze("macro-signatures.gti", source);
  check(snapshot.semanticValid,
        "macro signature query source is semantically valid");
  const lang::LanguageQueries queries;
  for (const auto &[name, expected] :
       {std::pair{"implicit_void", "#macro implicit_void()"},
        std::pair{"explicit_void", "#macro void explicit_void()"},
        std::pair{"result", "#macro int32_t result(int32_t value)"},
        std::pair{"generic", "#macro T generic<T>(T value)"}}) {
    const auto hover = queries.hover(snapshot, snapshot.sourceGraph.entryUnit(),
                                     source.find(name) + 1);
    check(hover && hover->signature == expected,
          "macro signature queries preserve explicit versus omitted result "
          "types");
  }
}
} // namespace

int main() {
  signatures();
  recovery();
  signatureQueries();
  return failures == 0 ? 0 : 1;
}
