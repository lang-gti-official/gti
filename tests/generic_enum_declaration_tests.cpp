#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/lexer.h"
#include "gti/parser.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testDeclarationSyntax() {
  lang::Lexer lexer;
  lang::Parser parser(
      lexer.scan("enum class Message<T, U = T, uint64_t N = 3> { empty, "
                 "item(T value, U detail), address(const T* data) };",
                 "generic-enum-syntax.gti"));
  const auto program = parser.parse();
  expect(!lexer.hadError() && !parser.hadError(),
         "enum declarations reuse ordinary generic parameter syntax");
  const auto *declaration =
      program.declarations().empty()
          ? nullptr
          : lang::stmtAs<lang::EnumDecl>(program.declarations().front().get());
  expect(declaration && declaration->genericParameters().size() == 3 &&
             declaration->enumerators().size() == 3,
         "AST retains both binders and alternatives");
  if (declaration && declaration->genericParameters().size() == 3) {
    const auto &parameters = declaration->genericParameters();
    expect(parameters[0].name.lexeme == "T" && parameters[1].defaultType &&
               parameters[1].defaultType->name.last().lexeme == "T" &&
               parameters[2].valueType && parameters[2].defaultValue,
           "type and value defaults retain their ordinary AST identity");
  }
}

void testDependentPayloads() {
  auto result = lang::Frontend().analyze("generic-enum-declaration.gti", R"(
struct Box<T> { T value; };
enum class Message<T, U = T> {
  empty,
  item(T value, U detail),
  boxed(Box<T> value)
};
int main() { return 0; }
)");
  for (const auto &diagnostic : result.diagnostics) {
    if (diagnostic.severity == lang::DiagnosticSeverity::Error)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(result.canGenerateCode(),
         "unused generic declarations retain dependent fields without "
         "emitting a symbolic enum representation");
  const auto *declaration =
      lang::stmtAs<lang::EnumDecl>(result.program.declarations()[1].get());
  const auto *info =
      declaration ? result.semantics.findEnumType(*declaration) : nullptr;
  expect(info && info->genericParameters.size() == 2 &&
             info->enumerators.size() == 3,
         "semantic enum catalog owns resolved generic binders");
  if (info && info->genericParameters.size() == 2 &&
      info->enumerators.size() == 3) {
    expect(info->enumerators[1].payloadTypes[0] ==
                   lang::SemanticType::typeParameter(
                       info->genericParameters[0].id) &&
               info->enumerators[1].payloadTypes[1] ==
                   lang::SemanticType::typeParameter(
                       info->genericParameters[1].id) &&
               info->enumerators[2].payloadTypes[0].arguments.front() ==
                   lang::SemanticType::typeParameter(
                       info->genericParameters[0].id),
           "payload signatures use the declaration's exact generic identities");
    expect(result.hir.enumDeclarations().empty(),
           "a generic primary is not an executable concrete enum instance");
  }
}

void testOptionalClassSpelling() {
  for (const std::string prefix : {"enum ", "enum class "}) {
    const auto source = prefix + R"(State : uint8_t { idle, ready = 4 };
)" + prefix + R"(Message { empty, item(int value) };
int main() {
  Message message = Message::item(7);
  switch (message) {
    case Message::empty: return 1;
    case Message::item(value): return value == 7 ? 0 : 2;
  }
}
)";
    const auto result =
        lang::Frontend().analyze("optional-enum-class.gti", source);
    expect(result.canGenerateCode(),
           "both spellings share integral and payload enum lowering");
    if (!result.canGenerateCode()) {
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    const auto *declaration = lang::stmtAs<lang::EnumDecl>(
        result.program.declarations().front().get());
    expect(declaration && declaration->classKeyword().has_value() ==
                              (prefix == "enum class "),
           "AST preserves the chosen spelling without changing meaning");
    const auto formatted = lang::Formatter().format(source);
    expect(formatted.starts_with(prefix + "State") &&
               lang::Formatter().format(formatted) == formatted &&
               lang::Frontend()
                   .analyze("formatted-enum.gti", formatted)
                   .canGenerateCode(),
           "formatter preserves either spelling and reanalysis is equivalent");
    for (const std::string &invalid :
         {prefix + "E { member }; int main() { E value = member; return 0; }",
          prefix + "E { member }; int main() { int value = E::member; return "
                   "value; }",
          prefix + "E { item(int value) }; int main() { E value = E::item(1); "
                   "switch (value) { } return 0; }"}) {
      expect(!lang::Frontend()
                  .analyze("bad-enum-spelling.gti", invalid)
                  .semanticValid,
             "optional class does not admit unqualified cases, integer "
             "conversions or nonexhaustive payload matching");
    }
  }
}

void testInvalidDeclarations() {
  for (const auto source :
       {"enum class Event<T, T> { item(T value) };",
        "enum class Event<T> { item(Missing value) };",
        "enum class Event<T> { item(T& value) };",
        "enum class Event<T> { item(mut T value) };",
        "enum class Event<T> { empty };",
        "enum class Event<T> { empty, item(T value) }; "
        "int main() { auto event = Event::empty; return 0; }",
        "enum class Event<T> { item(T value) }; "
        "int main() { auto event = Event::item(1); return 0; }"}) {
    const auto result =
        lang::Frontend().analyze("bad-generic-enum.gti", source);
    expect(result.syntaxValid && !result.semanticValid &&
               !result.canGenerateCode(),
           "invalid generic enum declarations or uninstantiated values fail "
           "before executable lowering");
  }
}

void testExactTypeIdentity() {
  const auto first =
      lang::SemanticType::enumType(1, {lang::SemanticType::Int32});
  const auto same =
      lang::SemanticType::enumType(1, {lang::SemanticType::Int32});
  const auto second =
      lang::SemanticType::enumType(1, {lang::SemanticType::Int64});
  const auto other =
      lang::SemanticType::enumType(2, {lang::SemanticType::Int32});
  expect(first == same && first != second && first != other,
         "generic enum type identity includes the declaration and arguments");
}

void testForwardDefaultsAndLatePayloadValidation() {
  for (const auto source : {
           "enum First<T, U = Later<int>> { item(T value) }; "
           "enum Later<T> { item(T value) }; "
           "int main() { auto event = First<int>::item(7); return 0; }",
           "enum Later<T> { item(T value) }; "
           "enum First<T, U = Later<int>> { item(T value) }; "
           "int main() { auto event = First<int>::item(7); return 0; }",
           "enum First<T = Box<int>> { item(T value) }; "
           "struct Box<T> { T value; }; int main() { return 0; }",
           "enum Event<T, uint64_t N = 3> { idle, item(T value) }; "
           "int read<T, uint64_t N>(Event<T, N>& value) { return 0; } "
           "int main() { auto event = Event<int>::item(7); return read(event); }",
           "struct Tagged<T, uint64_t N> { T value; Tagged(T input) : value(input) {} }; "
           "int read<T, uint64_t N>(Tagged<T, N>& value) { return 0; } "
           "int main() { Tagged<int, 3> value{7}; return read(value); }",
           "enum Event<T, Rest...> { item(T value) }; "
           "int read<T, Rest...>(Event<T, Rest...>& value) { return 0; } "
           "int main() { auto event = Event<int, bool>::item(7); return read(event); }"}) {
    const auto result = lang::Frontend().analyze("enum-defaults-and-inference.gti", source);
    expect(result.canGenerateCode(),
           "nominal defaults and type/value/pack inference share exact generic semantics");
    if (!result.canGenerateCode()) {
      std::cerr << source << '\n';
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  for (const auto source : {
           "enum Event<T> { idle, item(T value) }; using Invalid = Event<Event<int>>; "
           "int main() { Invalid value = Invalid::idle; return 0; }",
           "enum First<T = Later<>> { item(T value) }; "
           "enum Later<T = First<>> { item(T value) }; int main() { return 0; }"}) {
    const auto result = lang::Frontend().analyze("invalid-enum-alias-or-default.gti", source);
    expect(!result.semanticValid && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto &diagnostic) { return diagnostic.phase == lang::DiagnosticPhase::Semantics &&
                                           diagnostic.severity == lang::DiagnosticSeverity::Error; }),
        "invalid aliases and cyclic defaults reject before HIR/MIR/backend entry");
    expect(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto &diagnostic) { return diagnostic.code == "GTI-B0001"; }),
        "invalid nominal applications do not escape as compiler integrity errors");
  }
}
} // namespace

int main() {
  testDeclarationSyntax();
  testDependentPayloads();
  testOptionalClassSpelling();
  testInvalidDeclarations();
  testExactTypeIdentity();
  testForwardDefaultsAndLatePayloadValidation();
  return failures == 0 ? 0 : 1;
}
