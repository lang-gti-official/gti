#include "gti/frontend.h"
#include "gti/lifetime_types.h"

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

lang::FrontendResult analyze(std::string_view source) {
  return lang::Frontend().analyze("lifetime-resolution.gti", std::string(source));
}

void expectValid(const lang::FrontendResult &result, std::string_view message) {
  if (!result.semanticValid)
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.semanticValid, message);
}

const lang::FunctionInfo *function(const lang::FrontendResult &result,
                                    std::string_view name) {
  for (const auto &declaration : result.program.declarations())
    if (const auto *syntax = lang::stmtAs<lang::FunctionDecl>(declaration.get());
        syntax && syntax->name().lexeme == name)
      return result.semantics.findFunction(*syntax);
  return nullptr;
}

const lang::TypeAliasInfo *alias(const lang::FrontendResult &result,
                                 std::string_view name) {
  for (const auto &declaration : result.program.declarations())
    if (const auto *syntax = lang::stmtAs<lang::TypeAliasDecl>(declaration.get());
        syntax && syntax->name().lexeme == name)
      return result.semantics.findTypeAlias(*syntax);
  return nullptr;
}

void testNominalArguments() {
  const auto result = analyze(R"(
struct Marker<T, lifetime a> { T value; };
enum Message<T, lifetime a> { item(T value) };
using Label<lifetime a> = Marker<int, a>;
void inspect<lifetime a>(Label<a> first, Message<int, a> second);
int main() { return 0; }
)");
  expectValid(result, "nominal lifetime arguments and generic aliases resolve");
  const auto *info = function(result, "inspect");
  expect(info && info->genericParameters.size() == 1 &&
             info->genericParameters.front().lifetime &&
             info->parameterTypes.size() == 2,
         "lifetime binders retain a dedicated semantic kind");
  if (!info || info->genericParameters.empty() || info->parameterTypes.size() != 2)
    return;
  const auto region = lang::Lifetime::parameter(info->genericParameters[0].id);
  for (const auto &type : info->parameterTypes)
    expect(type.arguments.size() == 1 &&
               type.arguments[0] == lang::SemanticType::Int32 &&
               type.lifetimeArguments == std::vector{region},
           "regions do not occupy runtime type argument slots");
}

void testTransparentAliases() {
  const auto result = analyze(R"(
using Ref<lifetime a> = int&<a>;
using Elided = int&;
void inspect<lifetime a>(Ref<a> first, Elided second, Elided third);
int main() { return 0; }
)");
  expectValid(result, "explicit and elided reference aliases are transparent");
  const auto *elided = alias(result, "Elided");
  expect(elided && elided->genericParameters.size() == 1 &&
             elided->genericParameters[0].lifetime &&
             elided->type.referenceLifetime.kind == lang::Lifetime::Kind::Parameter,
         "an elided alias stores one finite anonymous declaration region");
  const auto *info = function(result, "inspect");
  if (!info || info->parameterTypes.size() != 3) {
    expect(false, "reference alias parameter metadata is available");
    return;
  }
  const auto &parameters = info->parameterTypes;
  expect(parameters[0].kind == lang::SemanticType::Reference &&
             parameters[1].kind == lang::SemanticType::Reference &&
             parameters[2].kind == lang::SemanticType::Reference &&
             parameters[1].referenceLifetime != parameters[2].referenceLifetime &&
             parameters[1].referenceLifetime.declarationRegion() &&
             parameters[2].referenceLifetime.declarationRegion(),
         "separate alias uses receive independent normalized lifetime slots");
}

void testHigherRankedAlias() {
  const auto result = analyze(R"(
using Callback = for<lifetime a> int&<a>(int&<a>);
int main() { return 0; }
)");
  expectValid(result, "higher-ranked aliases keep their local binder scope");
  const auto *info = alias(result, "Callback");
  expect(info && info->type.kind == lang::SemanticType::FunctionSignature &&
             info->type.quantifiedLifetimes.size() == 1 &&
             info->type.arguments.size() == 2,
         "higher-ranked callable metadata retains its quantified region");
  if (info && info->type.quantifiedLifetimes.size() == 1 &&
      info->type.arguments.size() == 2) {
    const auto region = lang::Lifetime::parameter(info->type.quantifiedLifetimes[0]);
    expect(info->type.arguments[0].referenceLifetime == region &&
               info->type.arguments[1].referenceLifetime == region &&
               lang::freeTypeLifetimes(info->type).empty(),
           "the result and input share a scoped region that does not escape");
  }
}

void testAssociatedLifetimeFamily() {
  const auto result = analyze(R"(
concept BorrowType<T, lifetime owner> { typename Borrow<lifetime a>; };
for<lifetime owner> concept BorrowType<int, owner> {
  using Borrow<lifetime a> = int&<a>;
};
using Result<lifetime a> = BorrowType<int, a>::Borrow<a>;
int main() { return 0; }
)");
  expectValid(result, "associated families bind and substitute lifetime arguments");
  const auto *info = alias(result, "Result");
  expect(info && info->genericParameters.size() == 1 &&
             info->type.kind == lang::SemanticType::Reference &&
             info->type.referenceLifetime ==
                 lang::Lifetime::parameter(info->genericParameters[0].id),
         "associated type reduction preserves the selected family region");
}

void testBinderDiagnostics() {
  for (const std::string source : {
           "void invalid(int&<missing> value); int main() { return 0; }",
           "void invalid(int&<this> value); int main() { return 0; }",
           "void invalid<lifetime a, lifetime a>(); int main() { return 0; }",
           "struct Owner<T> { using a = int; }; struct Tag<lifetime a> {}; void invalid<lifetime a>(Tag<Owner<int>::a> value); int main() { return 0; }",
           "struct Pair<T, lifetime a> {}; using Bad = Pair<static, int>; int main() { return 0; }"}) {
    const auto result = analyze(source);
    expect(!result.semanticValid &&
               std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2126";
                           }),
           "invalid lifetime names and parameter kinds produce binder diagnostics");
    expect(function(result, "main") != nullptr,
           "binder diagnostics preserve following declaration analysis");
  }
}

void testProtocolOperationBinders() {
  const auto result = analyze(R"(
concept Shared<T> {
  T&<a> borrow<lifetime a>(T&<a> value);
  T&<a> relay<lifetime a>(T&<a> value);
};
for concept Shared<int> {
  int&<b> borrow<lifetime b>(int&<b> value) { return value; }
  int& relay(int& value) { return value; }
};
T&<a> forward<T, lifetime a>(T&<a> value) requires Shared<T> {
  return Shared<T>::relay<a>(value);
}
int main() {
  int value = 7;
  int& selected = Shared<int>::borrow<_>(value);
  int& relayed = Shared<int>::relay<_>(selected);
  return forward(relayed) - 7;
}
)");
  expectValid(result, "explicit protocol lifetimes select named and elided implementation binders");
}

void testExplicitFunctionKinds() {
  const auto result = analyze(R"(
void mixed<T, lifetime a, uint64_t N, lifetime b>(T&<a> first, int&<b> second) {}
void collect<lifetime a, T, Args...>(T&<a> first, Args&<a>... rest) {}
void exercise<lifetime a, lifetime b>(int&<a> first, int&<b> second) {
  mixed<int, a, 3, b>(first, second);
  collect<a, int, int, int>(first, first, first);
}
int main() { int value = 7; exercise(value, value); return 0; }
)");
  expectValid(result, "explicit lifetime arguments retain mixed generic order and type packs");
  const auto *exercise = function(result, "exercise");
  if (!exercise || !exercise->declaration || !exercise->declaration->body()) {
    expect(false, "mixed-kind call metadata remains available");
    return;
  }
  std::size_t checked = 0;
  for (const auto &statement : exercise->declaration->body()->statements()) {
    const auto *expression = lang::stmtAs<lang::ExpressionStmt>(statement.get());
    const auto *call = expression
        ? lang::exprAs<lang::Call>(expression->expression().get()) : nullptr;
    const auto *selected = call ? result.semantics.findCall(*call) : nullptr;
    if (!selected)
      continue;
    expect(selected->lifetimeApplication.complete &&
               !selected->lifetimeApplication.bindings.empty(),
           "selected generic calls retain a checked lifetime application");
    if (checked == 0)
      expect(selected->typeArguments.size() == 1 &&
                 selected->valueArguments.size() == 1 &&
                 selected->lifetimeArguments.size() == 2,
             "mixed kinds use separate compact runtime and lifetime vectors");
    else
      expect(selected->typeArguments.size() == 3 &&
                 selected->valueArguments.empty() &&
                 selected->lifetimeArguments.size() == 1 &&
                 selected->parameterTypes.size() == 3,
             "a leading lifetime does not count toward fixed types or pack elements");
    ++checked;
  }
  expect(checked == 2, "both explicit generic calls publish semantic metadata");

  for (const std::string source : {
           "void use<T, lifetime a>(T&<a> value) {} int main() { int x = 0; use<static, int>(x); return 0; }",
           "void use<lifetime a>(int&<a> value) {} int main() { int x = 0; use<missing>(x); return 0; }"}) {
    const auto invalid = analyze(source);
    expect(!invalid.semanticValid &&
               std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2126";
                           }),
           "explicit call arguments reject wrong kinds and unknown lifetime names");
  }
}
} // namespace

int main() {
  testNominalArguments();
  testTransparentAliases();
  testHigherRankedAlias();
  testAssociatedLifetimeFamily();
  testBinderDiagnostics();
  testProtocolOperationBinders();
  testExplicitFunctionKinds();
  return failures == 0 ? 0 : 1;
}
