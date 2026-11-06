#include "gti/frontend.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

using namespace lang;
namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
void constructionLifetimeContexts() {
  const auto result = Frontend({.stopAfter = FrontendPhase::Hir})
      .analyze("lifetime-hir-construction-contexts.gti", R"gti(
struct Token<T> { Token() = default; };
struct Builder<lifetime a, T> {
  Builder() { Token<T> token{}; }
  int make() { Token<T> token{}; return 1; }
};
struct Projected<T&<borrowed>> {
  Projected() = default;
  int make() { Token<T> token{}; return 1; }
};
namespace family { struct Exact<T, lifetime a> { Exact() = default; }; }
struct family::Exact<int> {
  Exact() = default;
  int make() { Token<int> token{}; return 1; }
};
int build<lifetime a, T>(T&<a> input) { Token<T> token{}; return 1; }
int first<lifetime a>(int&<a> input) {
  auto invoke = []() -> int { return 1; };
  return invoke();
}
int second<lifetime b>(int&<b> input) {
  auto invoke = []() -> int { return 1; };
  return invoke();
}
int main() {
  int value = 0;
  Builder<static, int> builder{};
  Projected<int&<static>> projected{};
  family::Exact<int, static> exact{};
  return build(value) + builder.make() + projected.make() + exact.make() + first(value) + second(value) - 6;
}
)gti");
  if (!result.semanticValid || !result.hir.valid()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "mixed-kind callable and owner construction contexts preserve compact arguments");
  }
  expect(verifyHirProgramPlans(result.semantics, result.hir).valid(),
         "construction context source proof accepts exact canonical lifetime substitutions");
  auto malformed = result.hir;
  HirValue *firstCreator = nullptr;
  HirValue *secondCreator = nullptr;
  for (const auto &function : malformed.functionInstances()) {
    if (!function.source)
      continue;
    const auto &name = function.source->name().lexeme;
    if (name != "first" && name != "second")
      continue;
    for (const auto &value : function.body.values)
      if (value.kind == HirValueKind::Lambda) {
        auto *creator = const_cast<HirValue *>(&value);
        (name == "first" ? firstCreator : secondCreator) = creator;
      }
  }
  expect(firstCreator && secondCreator, "context mutation fixture has two exact lexical creators");
  // Make the same checked lambda body claim two parent scopes. Their compact
  // type/value arguments agree, but their declaration lifetime identities do not.
  secondCreator->lambdaTarget = firstCreator->lambdaTarget;
  secondCreator->source = firstCreator->source;
  secondCreator->info = firstCreator->info;
  const auto rejected = verifyHirProgramPlans(result.semantics, malformed);
  expect(std::find(rejected.errors.begin(), rejected.errors.end(),
                   "construction body has ambiguous instance arguments") != rejected.errors.end(),
         "a lambda construction body cannot inherit unequal lifetime contexts");
}
} // namespace
int main() {
  constructionLifetimeContexts();
  const auto result = Frontend({.stopAfter = FrontendPhase::Hir})
                          .analyze("lifetime-hir-signatures.gti", R"gti(
T&<a> identity<lifetime a, T>(T&<a> value) { return value; }
int&<a_first> first<lifetime a_first>(int&<a_first> value) {
  return identity<a_first, int>(value);
}
int&<a_second> second<lifetime a_second>(int&<a_second> value) {
  return identity<a_second, int>(value);
}
struct Box<lifetime owner, T> { T value; Box(T initial) : value(initial) {} };
int main() {
  Box<static, int> box{0};
  int value = 17;
  return first(value) + second(value) - 34;
}
)gti");
  if (!result.semanticValid || !result.hir.valid()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "universal-signature fixture must lower through HIR");
  }
  const FunctionInfo *declaration = nullptr;
  for (const auto &statement : result.program.declarations()) {
    const auto *function = dynamic_cast<const FunctionDecl *>(statement.get());
    if (function && function->name().lexeme == "identity")
      declaration = result.semantics.findFunction(*function);
  }
  expect(declaration && declaration->lifetimeContract.valid(),
         "generic identity must have a complete declaration lifetime contract");
  const HirFunctionInstance *instance = nullptr;
  std::size_t count = 0;
  for (const auto &candidate : result.hir.functionInstances())
    if (candidate.declaration == declaration->id) {
      instance = &candidate;
      ++count;
    }
  expect(count == 1 && instance, "different caller regions must share one "
                                 "executable declaration instance");
  expect(instance->lifetimeContract.parameters ==
             declaration->lifetimeContract.parameters,
         "executable contract must retain declaration universal identities");
  expect(instance->returnType.referenceLifetime ==
                 declaration->returnType.referenceLifetime &&
             instance->parameterTypes.size() == 1 &&
             instance->parameterTypes[0].referenceLifetime ==
                 declaration->parameterTypes[0].referenceLifetime,
         "first caller regions must not replace universal return and input "
         "regions");
  expect(instance->returnType.arguments ==
                 std::vector<SemanticType>{SemanticType::Int32} &&
             instance->typeArguments ==
                 std::vector<SemanticType>{SemanticType::Int32},
         "lifetime binders must not consume compact executable type arguments");
  expect(
      !instance->lifetimeContract.typeBounds.empty(),
      "the exact implied referent-outlives obligation must remain available");
  for (const auto &bound : instance->lifetimeContract.typeBounds)
    expect(bound.type == SemanticType::Int32,
           "concrete HIR contracts must substitute exact type-outlives bound "
           "types");
  bool foundBox = false;
  for (const auto &candidate : result.hir.classInstances())
    if (candidate.source && candidate.source->name().lexeme == "Box") {
      foundBox = true;
      expect(candidate.typeArguments ==
                     std::vector<SemanticType>{SemanticType::Int32} &&
                 candidate.fields.size() == 1 &&
                 candidate.fields[0].info.type == SemanticType::Int32,
             "class lifetime binders must not shift concrete field type "
             "substitutions");
    }
  expect(foundBox, "the mixed-kind user-defined class must be instantiated");
  std::cout << "universal HIR lifetime signatures passed\n";
}
