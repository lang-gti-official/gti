#include "compiler_test_fixture.h"

namespace {
lang::FrontendResult analyzeMacroCallable(const std::string &source) {
  return lang::Frontend().analyze("macro-fresh-callables.gti", source,
                                  {standardLibraryPrelude()}, {},
                                  {standardLibraryRoot()});
}

void valid(const std::string &source, const std::string &message,
           std::optional<std::size_t> expectedLambdas = std::nullopt) {
  const auto result = analyzeMacroCallable(source);
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.canGenerateCode(), message);
  if (!result.canGenerateCode())
    return;
  expect(lang::verifyMirProgram(result.mir).valid(),
         message + " preserves verified closure operations");
  if (expectedLambdas)
    expect(result.hir.lambdaInstances().size() == *expectedLambdas,
           message + " preserves the expected lexical/concrete identities");
}

void rejected(const std::string &source, std::string_view diagnostic,
              const std::string &message) {
  const auto result = analyzeMacroCallable(source);
  expect(!result.semanticValid, message);
  expect(
      std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                  [&](const auto &found) { return found.code == diagnostic; }),
      message + " uses the ordinary ownership/loan diagnostic");
}

void tests() {
  valid(R"(
mut int total = 0;
#macro twice<F>(F callback) { callback(); callback(); }
#macro invoke<F>(F callback) { callback(); }
#macro nested<F>(F callback) { invoke(callback); }
int main() {
  int value = 7;
  twice([value]() -> void { total += value; });
  nested([]() -> void { total += 1; });
  auto named = []() -> void { total += 2; };
  invoke(named);
  return total == 17 ? 0 : 1;
}
)",
        "repeated and nested forcing shares its written lambda, while another "
        "lambda and named callable remain distinct",
        3);

  valid(R"(
#macro invoke<F>(F callback) { callback(); }
void observe(int value) {}
void observe(bool value) {}
void generic<T>(T value) {
  invoke([value]() -> void { observe(value); });
}
int main() { generic(1); generic(true); return 0; }
)",
        "one lexical lambda retains distinct enclosing concrete generic types",
        2);

  valid(R"(
mut int total = 0;
#macro invoke<F>(F callback) { callback(); }
int main() {
  invoke([]() -> void {
    invoke([]() -> void { total += 1; });
  });
  return total == 1 ? 0 : 1;
}
)",
        "nested lambda execution bodies have independent macro result regions",
        2);

  valid(R"(
#include <std/memory>
#macro ignore<F>(F callback) {}
#macro when<F>(bool enabled, F callback) { if (enabled) { callback(); } }
int main() {
  auto owner = std::make_unique<int>(7);
  ignore([owned = std::move(owner)]() -> void { int value = *owned; });
  when(false, [owned = std::move(owner)]() -> void { int value = *owned; });
  return *owner == 7 ? 0 : 1;
}
)",
        "unused and disabled captures establish no caller moves or cleanup");

  rejected(R"(
#include <std/memory>
#macro twice<F>(F callback) { callback(); callback(); }
int main() {
  auto owner = std::make_unique<int>(7);
  twice([owned = std::move(owner)]() -> void { int value = *owned; });
  return 0;
}
)",
           "GTI-S2018",
           "repeated closure construction cannot consume one owner twice");

  rejected(R"(
#include <std/memory>
#macro invoke<F>(F callback) { callback(); }
int main() {
  auto owner = std::make_unique<int>(7);
  auto& held = owner;
  invoke([owned = std::move(owner)]() -> void { int value = *owned; });
  return *held;
}
)",
           "GTI-S2017", "a forced move capture respects a live checked borrow");

  rejected(R"(
#macro ignore<F>(F callback) {}
int main() {
  ignore([]() -> int { return missing; });
  return 0;
}
)",
           "GTI-S2001", "unused fresh lambdas still type-check their bodies");
}
} // namespace

int main() {
  tests();
  return failures == 0 ? 0 : 1;
}
