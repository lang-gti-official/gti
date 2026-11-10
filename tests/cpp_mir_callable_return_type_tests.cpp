#include "../src/compiler/cpp_mir_representation_snapshot.h"
#include "compiler_test_fixture.h"
#include "cpp_backend_test_support.h"

namespace {
lang::FrontendResult analyze(std::string source) {
  return lang::Frontend().analyze("callable-return-type.gti", std::move(source),
                                  {standardLibraryPrelude()}, {},
                                  {standardLibraryRoot()});
}
} // namespace

int main() {
  const auto projected = analyze(R"(
import std.meta;
std::meta::callable_return_type<F> apply<F>(F& fn, int value) {
  return fn(value);
}
int main() {
  auto mapper = [](int value) -> int { return value; };
  return apply(mapper, 0);
}
)");
  expect(projected.canGenerateCode(), "projection mutation fixture is valid");
  if (projected.canGenerateCode()) {
    auto lowered =
        gti_test::lowerProgram(projected, projected.mir, projected.mir);
    bool signatureChecked = false;
    for (const auto &function : projected.mir.functionInstances()) {
      const auto *declaration =
          lowered.findFunctionDeclaration(function.declaration);
      if (!declaration || declaration->genericParameters.empty())
        continue;
      const auto signature = lang::cppMirCallableTemplateSignature(
          lowered, *declaration, function);
      if (!signature)
        continue;
      signatureChecked = true;
      auto substituted = function;
      substituted.returnType = lang::SemanticType::Bool;
      expect(!lang::cppMirCallableTemplateSignature(lowered, *declaration,
                                                    substituted),
             "projection cannot justify a substituted concrete MIR result");
    }
    expect(signatureChecked,
           "dependent callable signature is checked against MIR");
  }
  return failures == 0 ? 0 : 1;
}
