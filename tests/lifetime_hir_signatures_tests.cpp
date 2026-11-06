#include "gti/frontend.h"

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
} // namespace
int main() {
  const auto result = Frontend({.stopAfter = FrontendPhase::Hir})
                          .analyze("lifetime-hir-signatures.gti", R"gti(
T&<a> identity<lifetime a, T>(T&<a> value) { return value; }
int&<a_first> first<lifetime a_first>(int&<a_first> value) {
  return identity<a_first, int>(value);
}
int&<a_second> second<lifetime a_second>(int&<a_second> value) {
  return identity<a_second, int>(value);
}
struct Box<lifetime owner, T> { T value; };
int main() {
  Box<static, int> box{};
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
