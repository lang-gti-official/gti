#include "cpp_backend_test_support.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void expect(bool value, const std::string &message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void rejected(const std::string &source, const std::string &fragment) {
  const auto result =
      lang::Frontend().analyze("designated-invalid.gti", source);
  expect(!result.canGenerateCode(),
         "invalid construction must stop compilation");
  const auto found = std::find_if(
      result.diagnostics.begin(), result.diagnostics.end(), [&](const auto &d) {
        return d.code == "GTI-S2091" &&
               d.message.find(fragment) != std::string::npos;
      });
  expect(found != result.diagnostics.end(), "diagnostic: " + fragment);
  if (found != result.diagnostics.end()) {
    expect(found->primary.source.ends_with("/designated-invalid.gti") &&
               found->primary.start < found->primary.end &&
               found->primary.end <= source.size() && found->fixes.empty(),
           "diagnostic must have a real source span and no speculative fix");
  }
}
} // namespace

int main() {
  const std::string source = R"(
struct Point { int x = 3; int y = 4;  public: Point() = default; };
struct Required { int value; };
struct Nested { Point point{}; int z = 5;  public: Nested() = default; };
int main() {
  Point point = {.x = 7, .y = 8};
  Point defaults{.y = 9};
  Required required = {.value = 11};
  Nested nested = {.point = {.x = 12}};
  return point.x + defaults.x + required.value + nested.z;
}
)";
  auto result = lang::Frontend().analyze("designated.gti", source);
  for (const auto &d : result.diagnostics)
    std::cerr << d.code << ": " << d.message << '\n';
  expect(result.canGenerateCode(),
         "named fields, defaults, required fields and nesting");
  const std::string formatted = lang::Formatter().format(source);
  expect(lang::Frontend().analyze("formatted.gti", formatted).canGenerateCode(),
         "formatting preserves designated construction");
  expect(lang::Formatter().format(formatted) == formatted,
         "formatting is idempotent");
  if (result.canGenerateCode()) {
    expect(lang::verifyMirProgram(result.mir).valid(),
           "construction MIR verifies");
    const auto text = gti_test::emitCppText(result);
    expect(!text.empty(),
           "designated construction reaches the production backend");
    auto forged = result.mir;
    bool mutated = false;
    for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
             forged.functionInstances())) {
      for (auto &block : function.body.blocks) {
        for (auto &instruction : block.instructions) {
          if (!mutated &&
              instruction.kind == lang::MirInstructionKind::Construct &&
              instruction.constructorTarget &&
              !instruction.parameterTypes.empty()) {
            instruction.parameterTypes.front() = lang::SemanticType::Bool;
            mutated = true;
          }
        }
      }
    }
    expect(mutated && !lang::verifyMirProgram(forged).valid(),
           "forged construction parameter type must fail MIR verification");
  }
  rejected("struct S { int x = 0;  public: S() = default; }; int main(){ S s = "
           "{.bad = 1}; return 0; }",
           "Unknown instance field");
  rejected("struct S { uint64_t x = 0;  public: S() = default; }; int main(){ "
           "uint32_t n = 9; "
           "S s = {.x = n}; return 0; }",
           "Cannot initialize");
  rejected("struct S { int x = 0;  public: S() = default; }; int main(){ S s = "
           "{.x = 1, .x = 2}; "
           "return 0; }",
           "Duplicate");
  rejected("struct S { int x = 0; int y = 0;  public: S() = default; }; int "
           "main(){ S s = {.y = 1, .x "
           "= 2}; return 0; }",
           "declaration order");
  rejected(
      "struct S { int x; int y; }; int main(){ S s = {.x = 1}; return 0; }",
      "Missing initializer");
  rejected("struct S { int x = 0;  public: S() = default; }; int main(){ S s = "
           "{.x = true}; return 0; }",
           "Cannot initialize");
  rejected("class S { public: int x = 0;  public: S() = default; }; int "
           "main(){ S s = {.x = 1}; return 0; }",
           "requires a struct");
  rejected(
      "struct S { int x = 0; S() {} }; int main(){ S s = {.x = 1}; return 0; }",
      "no constructors other than a structural default");
  rejected("struct S { private: int x = 0;  public: S() = default; }; int "
           "main(){ S s = {.x = 1}; "
           "return 0; }",
           "public value fields");
  rejected("struct B { int x = 0;  public: B() = default; }; struct S : B { "
           "int y = 0;  public: S() = default; }; int main(){ "
           "S s = {.y = 1}; return 0; }",
           "no inheritance");
  rejected("class Owner { public: int x; Owner(int n):x(n){} ~Owner(){} }; "
           "struct S { Owner value; }; int main(){ Owner owner = Owner(1); S s "
           "= {.value = owner}; return 0; }",
           "Cannot initialize");
  const std::string missingDestination =
      "struct Point { int x; }; int read(Point value) { return value.x; } "
      "int main() { /* \xF0\x9F\x98\x80 */ return read({.x = 1}); }";
  const auto missing =
      lang::Frontend().analyze("missing-destination.gti", missingDestination);
  const auto error =
      std::find_if(missing.diagnostics.begin(), missing.diagnostics.end(),
                   [](const auto &d) { return d.code == "GTI-S2091"; });
  expect(!missing.canGenerateCode() && error != missing.diagnostics.end(),
         "an untyped designated argument must explain the missing context");
  if (error != missing.diagnostics.end()) {
    const auto brace = missingDestination.find("{.x");
    expect(
        error->phase == lang::DiagnosticPhase::Semantics &&
            error->severity == lang::DiagnosticSeverity::Error &&
            error->primary.source.ends_with("/missing-destination.gti") &&
            error->primary.start == brace && error->primary.end == brace + 1 &&
            error->message.find("destination struct type") !=
                std::string::npos &&
            error->message.find("Designated construction requires") ==
                std::string::npos &&
            error->hints.size() == 1 &&
            error->hints.front().find("explicit struct type") !=
                std::string::npos &&
            error->fixes.empty(),
        "the missing-context error has exact source bytes and no guessed fix");
  }
  expect(
      lang::Frontend()
          .analyze("bound-destination.gti",
                   "struct Point { int x; }; "
                   "int read(Point value) { return value.x; } "
                   "int main() { Point value = {.x = 1}; return read(value); }")
          .canGenerateCode(),
      "an explicitly typed binding supplies the required struct context");
  rejected("int main() { int value = {.x = 1}; return value; }",
           "requires a struct");
  rejected("struct Point { int x; }; void read(Point value) {} "
           "int main() { read({.x = 1}); return 0; }",
           "destination struct type");
  return failures == 0 ? 0 : 1;
}
