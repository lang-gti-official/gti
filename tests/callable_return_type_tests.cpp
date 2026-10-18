#include "../src/compiler/cpp_mir_representation_snapshot.h"
#include "compiler_test_fixture.h"
#include "gti/mir_editor.h"

namespace lang {
struct MirProgramTestAccess {
  static std::vector<MirFunctionInstance> &functions(MirProgram &program) {
    return program.functions;
  }
};
struct LoweredProgramTestAccess {
  static std::vector<LoweredDeclaration> &
  declarations(LoweredProgram &program) {
    return program.declarations_;
  }
};
} // namespace lang

namespace {
lang::FrontendResult analyze(std::string source) {
  return lang::Frontend().analyze("callable-return-type.gti", std::move(source),
                                  {standardLibraryPrelude()}, {},
                                  {standardLibraryRoot()});
}

void accepted(std::string source, const char *message) {
  const auto result = analyze(std::move(source));
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.canGenerateCode(), message);
  if (result.canGenerateCode()) {
    expect(lang::verifyMirProgram(result.mir).valid(),
           "query preserves valid MIR");
    expect(!gti_test::emitCpp(result).contents.empty(),
           "query reaches native emission");
  }
}

void rejected(std::string source, std::string_view code,
              std::string_view fragment) {
  const auto result = analyze(std::move(source));
  expect(!result.canGenerateCode(),
         "invalid query/use is rejected before emission");
  const bool found = std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(),
      [&](const auto &diagnostic) {
        return diagnostic.code == code &&
               diagnostic.message.find(fragment) != std::string::npos;
      });
  if (!found) {
    std::cerr << "Expected " << code << ": " << fragment << '\n';
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(found, "query/use has its semantic diagnostic");
}
} // namespace

int main() {
  accepted(
      R"(
#include <std/meta>
std::meta::callable_return_type<F> apply<F>(F fn, int value) { return fn(value); }
int increment(int value) { return value + 1; }
int main() { return apply(increment, 2) - 3; }
)",
      "selected function items are inferred before dependent result checking");
  accepted(
      R"(
#include <std/meta>
using Native = (int32_t) -> int32_t;
using NativeResult = std::meta::callable_return_type<Native>;
NativeResult zero() { return 0; }
int main() { return zero(); }
)",
      "native pointer result metadata does not require invoking the pointer");
  accepted(R"(
#include <std/meta>
using Number = std::meta::callable_return_type<int(int)>;
using Empty = std::meta::callable_return_type<void()>;
using Shared = std::meta::callable_return_type<int&(int&)>;
using MutableAction = std::meta::callable_return_type<void() mut>;
Number identity(Number value) { return value; }
Empty nothing() { return; }
int main() { nothing(); return identity(0); }
)",
           "signature results preserve scalar, void and reference metadata");
  accepted(R"(
#include <std/meta>
std::meta::callable_return_type<F> apply<F, T>(F& fn, T value) {
  return fn(value);
}
int plus_one(int value) { return value + 1; }
int main() {
  auto mapper = [](int value) -> int { return value * 2; };
  auto function = plus_one;
  return apply(mapper, apply(function, 2)) - 6;
}
)",
           "generic result query follows exact lambda and selected function "
           "item types");
  accepted(R"(
#include <std/meta>
std::meta::callable_return_type<F> invoke<F>(F& fn) { return fn(); }
void nothing() {}
int main() { auto function = nothing; invoke(function); return 0; }
)",
           "void result queries compose with ordinary void return expressions");
  rejected(R"(
#include <std/meta>
using Native = (int32_t) -> int32_t;
std::meta::callable_return_type<Native> invoke(Native fn) { return fn(1); }
int main() { return 0; }
)",
           "GTI-S2055", "unsafe");
  rejected(R"(
#include <std/meta>
using Invalid = std::meta::callable_return_type<int>;
int main() { return 0; }
)",
           "GTI-S2023", "Callable return-type reflection requires");
  rejected(R"(
#include <std/meta>
class Overloaded { public: int operator()(int x) { return x; } };
using Invalid = std::meta::callable_return_type<Overloaded>;
int main() { return 0; }
)",
           "GTI-S2023", "does not select an overloaded operator()");
  rejected(R"(
#include <std/meta>
struct Holder<F> { std::meta::callable_return_type<F> value; };
int main() { Holder<int> holder{}; return 0; }
)",
           "GTI-S2023", "Callable return-type reflection requires");
  rejected(R"(
#include <std/meta>
F escape<F>(F& fn) { return fn; }
int main() {
  int owner = 7;
  auto callback = [&owner]() -> int { return owner; };
  auto escaped = escape(callback);
  return escaped();
}
)",
           "GTI-S2046", "escape through");
  rejected(R"(
@compiler_type("meta_callable_return_type") struct Forged<F> {};
int main() { return 0; }
)",
           "GTI-S2049", "trusted standard-library unit");
  rejected(R"(
#include <std/meta>
std::meta::callable_return_type<F> bad<F>(F& fn) { return true; }
int mapper() { return 1; }
int main() { return bad(mapper); }
)",
           "GTI-S2003", "return");
  const auto projected = analyze(R"(
#include <std/meta>
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
    expect(lang::verifyLoweredProgram(lowered).empty(),
           "projection lowers coherently");
    bool found = false;
    for (auto &declaration :
         lang::LoweredProgramTestAccess::declarations(lowered)) {
      auto *type =
          std::get_if<lang::LoweredClassDeclaration>(&declaration.payload);
      if (!type || type->computedTypeProjection !=
                       lang::ComputedTypeProjectionKind::CallableReturn)
        continue;
      found = true;
      type->genericParameters.front().pack = true;
      expect(!lang::verifyLoweredProgram(lowered).empty(),
             "malformed pack projection is rejected");
      type->genericParameters.front().pack = false;
      type->computedTypeProjection =
          static_cast<lang::ComputedTypeProjectionKind>(255);
      expect(!lang::verifyLoweredProgram(lowered).empty(),
             "unknown projection kind is rejected");
      type->computedTypeProjection =
          lang::ComputedTypeProjectionKind::CallableReturn;
    }
    expect(found, "trusted projection retains declaration-bound metadata");
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
  const auto owned = analyze(R"(
#include <std/meta>
#include <std/string>
std::meta::callable_return_type<F> invoke<F>(F& mapper) {
  std::meta::callable_return_type<F> value = mapper();
  return value;
}
int main() {
  auto mapper = []() -> std::string { return std::string("owned"); };
  auto result = invoke(mapper);
  return result.size() == 5 ? 0 : 1;
}
)");
  expect(owned.canGenerateCode(), "class callback result initializes a local");
  if (owned.canGenerateCode()) {
    expect(!gti_test::emitCpp(owned).contents.empty(),
           "class callback publishes into its checked slot");
    bool checked = false;
    for (const auto &function : owned.mir.functionInstances()) {
      for (const auto &block : function.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Initialize ||
              !instruction.destination || instruction.operands.size() != 1 ||
              instruction.operands.front().type.kind !=
                  lang::SemanticType::Class)
            continue;
          auto malformed = owned.mir;
          auto &functions = lang::MirProgramTestAccess::functions(malformed);
          auto &changed = *std::find_if(
              functions.begin(), functions.end(),
              [&](const auto &item) { return item.id == function.id; });
          auto *destination = const_cast<lang::MirPlace *>(
              changed.body.findPlace(*instruction.destination));
          if (!destination)
            continue;
          destination->type = lang::SemanticType::Bool;
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "callback publication cannot substitute an incompatible "
                 "destination");
          checked = true;
        }
      }
    }
    expect(checked,
           "class publication mutation exercised an actual destination");
  }
  return failures == 0 ? 0 : 1;
}
