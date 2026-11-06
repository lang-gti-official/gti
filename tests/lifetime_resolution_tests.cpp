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
concept BorrowType<T, lifetime owner> { using Borrow<lifetime a>; };
for<lifetime owner> concept BorrowType<int, owner> {
  using Borrow<lifetime a> = int&<a>;
}
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
} // namespace

int main() {
  testNominalArguments();
  testTransparentAliases();
  testHigherRankedAlias();
  testAssociatedLifetimeFamily();
  testBinderDiagnostics();
  return failures == 0 ? 0 : 1;
}
