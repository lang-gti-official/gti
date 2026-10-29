#include "gti/ast_clone.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <filesystem>
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

lang::FrontendResult analyze(std::string source) {
  const auto stdlib =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze("unsafe-expression.gti", std::move(source),
                                  {stdlib / "prelude.gti"}, {}, {stdlib});
}

void rejected(std::string source, std::string_view label,
              std::string_view code = {}) {
  const auto result = analyze(std::move(source));
  expect(!result.canGenerateCode(), label);
  expect(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto &d) { return d.code == "GTI-B0001"; }),
         "invalid input must be diagnosed before the backend");
  if (!code.empty())
    expect(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [&](const auto &d) { return d.code == code; }),
           label);
}

void testPermissionAndRepresentation() {
  const std::string source = R"(
unsafe T identity<T>(T value) { return value; }
T permitted<T>(T value) { return unsafe { identity(value) }; }
unsafe int read(const int* p) { return unsafe { *p }; }
struct Item {
  int value;
  unsafe Item(int n) : value(n) {}
};
int add(int a, int b) { return a + b; }
int main() {
  int n = 20;
  auto item = unsafe { Item(read(&n)) };
  auto result = add(unsafe { permitted(item.value) }, unsafe { read(&n) });
  return result == 40 ? 0 : 1;
}
)";
  const auto result = analyze(source);
  for (const auto &d : result.diagnostics)
    std::cerr << d.code << ": " << d.message << '\n';
  expect(result.canGenerateCode(),
         "generic calls, constructors, arguments and returns");
  if (!result.canGenerateCode())
    return;
  const auto cpp = gti_test::emitCppText(result);
  expect(cpp.find("// GTI verified-MIR body:") != std::string::npos,
         "execution uses the production verified MIR emitter");
  auto forged = result.mir;
  bool changed = false;
  for (auto &fn : const_cast<std::vector<lang::MirFunctionInstance> &>(
           forged.functionInstances())) {
    for (auto &block : fn.body.blocks) {
      for (auto &instruction : block.instructions) {
        if (!changed && instruction.unsafeOperation ==
                            lang::UnsafeOperationKind::DeclaredUnsafeCall) {
          instruction.unsafeOperation = lang::UnsafeOperationKind::None;
          changed = true;
        }
      }
    }
  }
  expect(changed && !lang::verifyMirProgram(forged).valid(),
         "expression syntax cannot erase MIR unsafe-operation proofs");
  const auto formatted = lang::Formatter().format(source);
  expect(lang::Formatter().format(formatted) == formatted,
         "unsafe expression formatting is idempotent");
  expect(analyze(formatted).canGenerateCode(),
         "formatting preserves unsafe expression meaning");
}

void testBoundaries() {
  const std::string prefix = "unsafe int raw() { return 7; }\n";
  rejected(prefix + "int main() { auto x = unsafe { raw() }; return raw(); }",
           "permission ends at closing brace", "GTI-S2055");
  rejected(prefix + "int main() { return unsafe { raw() } + raw(); }",
           "permission cannot leak to adjacent operands", "GTI-S2055");
  rejected(prefix + "int add(int a, int b) { return a + b; } "
                    "int main() { return add(unsafe { raw() }, raw()); }",
           "permission cannot leak to later arguments", "GTI-S2055");
  rejected(prefix + "int main() { auto f = unsafe { []() -> int { return "
                    "raw(); } }; return f(); }",
           "lambda bodies do not inherit permission", "GTI-S2055");
  rejected("int main() { return unsafe { int x = 1; x }; }",
           "value expressions do not hide statement scopes", "GTI-P0001");
  rejected("int main() { return unsafe { 1; }; }",
           "trailing semicolon is not a value", "GTI-P0001");
  rejected("int main() { return unsafe {}; }", "missing operand", "GTI-P0001");
  rejected("int main() { mut int x = 1; unsafe { x } = 2; return x; }",
           "statement syntax is not an assignment target", "GTI-P0001");
  rejected(
      "int& bad() { int x = 1; return unsafe { x }; } int main() { return 0; }",
      "unsafe does not allow a checked reference to a dead local", "GTI-S2017");
  rejected("int main() { int x = 1; mut int& ref = unsafe { x }; return ref; }",
           "unsafe does not upgrade checked reference access", "GTI-S2017");
  rejected(
      "import std.memory;\nint main() { auto p = std::make_unique<int>(1); "
      "auto q = unsafe { std::move(p) }; return *p; }",
      "ordinary use-after-move checks survive the wrapper", "GTI-S2018");
  rejected(
      "import std.memory;\nint main() { auto p = std::make_unique<int>(1); "
      "auto& ref = *p; auto q = unsafe { std::move(p) }; return ref; }",
      "unsafe cannot consume a checked borrowed owner", "GTI-S2017");
  rejected("import std.string;\nint main() { auto text = unsafe { "
           "std::string(\"abc\").view() }; "
           "return text.empty() ? 0 : 1; }",
           "temporary-owner borrow cannot escape", "GTI-S2045");

  const auto pure =
      analyze("constexpr int value() { return unsafe { 40 + 2 }; } "
              "static_assert(value() == 42); int main() { return value(); }");
  expect(pure.canGenerateCode(),
         "grouping preserves existing constant eligibility");
  bool warning = false;
  for (const auto &d : pure.diagnostics) {
    if (d.group == "unused-unsafe") {
      warning = true;
      expect(d.fixes.empty(),
             "never offer invalid keyword-only expression removal");
    }
  }
  expect(warning,
         "unnecessary expression permission retains advisory diagnostics");

  const auto contexts = analyze(prefix + R"(
int global = unsafe { raw() };
int defaulted(int n = unsafe { raw() }) { return n; }
struct Fixed {
  int value;
  unsafe Fixed() : value(unsafe { raw() }) {}
  Fixed(Fixed& other) = delete;
  Fixed(Fixed&& other) = delete;
};
int main() {
  auto fixed = unsafe { Fixed() };
  return fixed.value + defaulted() + global;
}
)");
  expect(contexts.canGenerateCode(),
         "global/default/field contexts and fresh immovable initialization");
  if (contexts.canGenerateCode())
    expect(!gti_test::emitCppText(contexts).empty(),
           "every admitted context has a production backend representation");
}

void testSyntaxClone() {
  lang::Token keyword{
      lang::TokenKind::UNSAFE, "unsafe", {}, 10, 1, "clone.gti"};
  lang::Token name{
      lang::TokenKind::IDENTIFIER, "value", {}, 19, 1, "clone.gti"};
  const lang::Grouping original(std::make_unique<lang::Variable>(name),
                                keyword);
  const auto copied = lang::cloneExpr(original, 7);
  const auto *group = lang::exprAs<lang::Grouping>(copied.get());
  expect(group && group->unsafeKeyword() &&
             group->unsafeKeyword()->expansion == 7 &&
             group->unsafeKeyword()->position == 10 &&
             group->unsafeKeyword()->source == "clone.gti",
         "concrete AST cloning preserves permission, location and expansion "
         "identity");
}
} // namespace

int main() {
  testPermissionAndRepresentation();
  testBoundaries();
  testSyntaxClone();
  return failures == 0 ? 0 : 1;
}
