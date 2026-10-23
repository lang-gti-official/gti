#include "gti/formatter.h"
#include "gti/frontend.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

namespace {
int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult check(const std::filesystem::path &root,
                           const std::filesystem::path &directory,
                           const std::string &name, const std::string &source) {
  const auto file = directory / (name + ".gti");
  std::ofstream(file) << source;
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Hir;
  options.analyzeRecoveredProgram = true;
  return lang::Frontend(options).analyze(file, std::nullopt,
                                         {root / "stdlib/prelude.gti"}, {},
                                         {root / "stdlib"});
}

void accepted(const lang::FrontendResult &result, const std::string &name) {
  const bool errors = std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(), [](const auto &item) {
        return item.severity == lang::DiagnosticSeverity::Error;
      });
  expect(!errors && result.hirValid, name);
  if (errors)
    for (const auto &item : result.diagnostics)
      std::cerr << "  " << item.code << ": " << item.message << '\n';
}

void rejected(const lang::FrontendResult &result, const std::string &name,
              const std::string &code = "GTI-S2029") {
  const auto diagnostic = std::find_if(
      result.diagnostics.begin(), result.diagnostics.end(), [&](const auto &item) {
        return item.code == code;
      });
  expect(diagnostic != result.diagnostics.end(), name + " diagnostic");
  if (diagnostic != result.diagnostics.end())
    expect(diagnostic->primary.line > 0, name + " source location");
  expect(!result.hirValid, name + " cannot publish executable HIR");
  expect(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto &item) { return item.code == "GTI-B0001"; }),
         name + " is not a backend error");
}
} // namespace

int main(int argc, char **argv) {
  const auto root = argc > 1 ? std::filesystem::path(argv[1])
                             : std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto directory = std::filesystem::temp_directory_path() /
                         ("gti-general-requires-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);

  accepted(check(root, directory, "operators", R"(
concept adds_to<L, R, V> = requires(L& left, R& right) {
  { left + right } -> V;
};
struct Count {
  int value = 0;
  int operator+(Count& other) { return this.value + other.value; }
};
V add<L, R, V>(L& left, R& right) requires adds_to<L, R, V> {
  return left + right;
}
int main() {
  static_assert(requires(int& a, int& b) {
    { a + b } -> int;
    { a == b } -> bool;
    { -a } -> int;
  });
  static_assert(!(requires(int& a) { { a + a } -> bool; }));
  Count a{.value = 3}; Count b{.value = 4};
  return add<Count, Count, int>(a, b) == 7 ? 0 : 1;
}
)"), "heterogeneous operator result and grouped checks");

  accepted(check(root, directory, "construction", R"(
concept defaults<T> = requires { { T() } -> T; };
concept builds<T, A> = requires(A& argument) { { T(argument) } -> T; };
class Box {
  int value;
public:
  Box(int& value) : value(value) {}
  int read() { return this.value; }
};
T make<T, A>(A& argument) requires builds<T, A> { return T(argument); }
T empty<T>() requires defaults<T> { return T(); }
int main() {
  int value = 9;
  Box result = make<Box, int>(value);
  return result.read() == 9 && empty<int>() == 0 ? 0 : 1;
}
)"), "default and argument-bearing construction");

  accepted(check(root, directory, "unevaluated", R"(
#include <std/memory>
mut int calls = 0;
int never_run() { calls += 1; return 1; }
int main() {
  static_assert(requires { { never_run() } -> int; });
  static_assert(requires(std::unique_ptr<int> input) {
    { std::move(input) } -> std::unique_ptr<int>;
  });
  static_assert(!(requires(std::unique_ptr<int>& input) {
    { std::move(input) } -> std::unique_ptr<int>;
  }));
  static_assert(!(requires(std::unique_ptr<int>& input) {
    { input } -> int;
  }));
  auto owner = std::make_unique<int>(7);
  constexpr bool can_move = requires(std::unique_ptr<int> input) {
    { std::move(input) } -> std::unique_ptr<int>;
  };
  return calls == 0 && can_move && *owner == 7 ? 0 : 1;
}
)"), "queries neither execute calls nor move caller objects");

  accepted(check(root, directory, "access", R"(
class Hidden {
  Hidden() {}
  int secret() { return 1; }
};
class Mutable {
public:
  int read() mut { return 1; }
  unsafe int raw() { return 2; }
};
int main() {
  static_assert(!(requires { { Hidden() } -> Hidden; }));
  static_assert(!(requires(Hidden& input) { { input.secret() } -> int; }));
  static_assert(!(requires(Mutable& input) { { input.read() } -> int; }));
  static_assert(requires(mut Mutable& input) { { input.read() } -> int; });
  static_assert(!(requires(Mutable& input) { { input.raw() } -> int; }));
  static_assert(requires(Mutable& input) { unsafe { input.raw() } -> int; });
  return 0;
}
)"), "private access, receiver mutability and explicit unsafe obligations");

  accepted(check(root, directory, "predicates", R"(
#include <std/meta>
constexpr bool integer_alias<T>() { return std::meta::is_integral<T>(); }
concept integer_value<T> = integer_alias<T>();
concept possible<T> = std::meta::is_integral<T>() || std::meta::is_floating_point<T>();
T twice<T>(T value) requires integer_value<T> { return value + value; }
T identity<T>(T value) requires possible<T> { return value; }
constexpr bool arbitrary<T>() { return std::meta::is_integral<T>() && (1 + 1 == 2); }
concept has_size<T> = arbitrary<T>();
int sized<T>() requires has_size<T> { return 1; }
int main() { return twice(4) == 8 && identity(3.0) == 3.0 && sized<int>() == 1 ? 0 : 1; }
)"), "reflection implication, disjunction and ordinary constexpr predicates");

  rejected(check(root, directory, "wrong_result", R"(
concept adds_bool<T> = requires(T& left, T& right) { { left + right } -> bool; };
int use<T>(T& value) requires adds_bool<T> { return 0; }
int main() { int value = 1; return use(value); }
)"), "exact result mismatch");

  rejected(check(root, directory, "false_predicate", R"(
#include <std/meta>
concept integral_only<T> = std::meta::is_integral<T>();
int use<T>(T value) requires integral_only<T> { return 0; }
int main() { return use(true); }
)"), "false reflected predicate");

  rejected(check(root, directory, "nonconstant", R"(
bool runtime_only<T>() { return true; }
concept invalid<T> = runtime_only<T>();
int use<T>() requires invalid<T> { return 0; }
int main() { return use<int>(); }
)"), "non-constexpr predicate");

  rejected(check(root, directory, "not_boolean", R"(
constexpr int not_bool<T>() { return 1; }
concept invalid<T> = not_bool<T>();
int use<T>() requires invalid<T> { return 0; }
int main() { return use<int>(); }
)"), "predicate must have exact bool type");

  rejected(check(root, directory, "alternative", R"(
concept first<T> = requires(T& value) { { value + value } -> int; };
concept second<T> = requires(T& value) { { value - value } -> bool; };
int broken<T>(T& value) requires first<T> || second<T> {
  return value + value;
}
int main() { int value = 1; return broken(value); }
)"), "alternative requirements cannot invent a shared operation", "GTI-S2004");

  const std::string formatting = R"(concept sum<T>=requires(T& a,T& b){{a+b}->T;{a==b}->bool;};
concept ints<T>=std::meta::is_integral<T>();
)";
  const auto formatted = lang::Formatter().format(formatting);
  expect(lang::Formatter().format(formatted) == formatted,
         "general requirement formatting is idempotent");
  std::filesystem::remove_all(directory);
  if (failures != 0)
    return 1;
  std::cout << "General requires semantic checks passed\n";
  return 0;
}
