#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/generic_constraint.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

namespace {
static_assert(!lang::compilerStructuralConstraint("adds"));
static_assert(!lang::compilerStructuralConstraint("subtracts"));
static_assert(!lang::compilerStructuralConstraint("negates"));
static_assert(!lang::compilerStructuralConstraint("scales_by"));
static_assert(!lang::compilerStructuralConstraint("enum_underlying"));
static_assert(!lang::compilerStructuralConstraint("object_layout"));
static_assert(!lang::compilerStructuralConstraint("independent_object"));
static_assert([] {
  for (const std::string_view binding :
       {"numeric", "signed_numeric", "integral", "signed_integral",
        "unsigned_integral", "floating_point", "copy_constructible",
        "move_constructible", "copyable", "movable", "relocatable",
        "bitwise_relocatable", "transferable", "shareable",
        "default_initializable", "integral_enum", "atomic_scalar",
        "meta_reflectable"})
    if (lang::compilerConstraint(binding))
      return false;
  return true;
}());
static_assert(lang::compilerConstraint("equality_comparable") ==
              lang::GenericConstraintKind::EqualityComparable);
static_assert(lang::compilerConstraint("relationally_ordered") ==
              lang::GenericConstraintKind::RelationallyOrdered);

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult check(const std::filesystem::path &root,
                           const std::filesystem::path &directory,
                           const std::string &name, const std::string &source,
                           bool concurrent = false) {
  const auto file = directory / (name + ".gti");
  std::ofstream(file) << source;
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Hir;
  options.analyzeRecoveredProgram = true;
  if (concurrent) {
    options.target.executionProfile = lang::ExecutionProfile::Concurrent;
    options.target.runtimeCapabilities.threads = true;
  }
  return lang::Frontend(options).analyze(
      file, std::nullopt, {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
}

void accepted(const lang::FrontendResult &result, const std::string &name) {
  const bool errors =
      std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                  [](const auto &item) {
                    return item.severity == lang::DiagnosticSeverity::Error;
                  });
  expect(!errors && result.hirValid, name);
  if (errors)
    for (const auto &item : result.diagnostics)
      std::cerr << "  " << item.code << " at " << item.primary.line << ": "
                << item.message << '\n';
}

void rejected(const lang::FrontendResult &result, const std::string &name,
              const std::string &code = "GTI-S2029") {
  const auto diagnostic =
      std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
                   [&](const auto &item) { return item.code == code; });
  expect(diagnostic != result.diagnostics.end(), name + " diagnostic");
  if (diagnostic != result.diagnostics.end())
    expect(diagnostic->primary.line > 0, name + " source location");
  expect(!result.hirValid, name + " cannot publish executable HIR");
  expect(
      std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                   [](const auto &item) { return item.code == "GTI-B0001"; }),
      name + " is not a backend error");
}
} // namespace

int main(int argc, char **argv) {
  const auto root =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto directory = std::filesystem::temp_directory_path() /
                         ("gti-general-requires-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);

  accepted(
      check(root, directory, "source_scalar_policy", R"(
#include <std/atomic>
#include <std/meta>
constexpr bool truth_type<T>() { return std::meta::is_bool<T>(); }
concept integer_or_boolean<T> = std::meta::is_integral<T>() || truth_type<T>();
concept signed_or_unsigned<T> = std::meta::is_signed_integral<T>() ||
                               std::meta::is_unsigned_integral<T>();
T load<T>(std::atomic<T>& value) requires integer_or_boolean<T> {
  return value.load();
}
T load_truth<T>(std::atomic<T>& value) requires truth_type<T>() {
  return value.load();
}
T increment<T>(T value) requires signed_or_unsigned<T> { return value + 1; }
struct Record { int value = 0; };
concept public_record<T> = std::meta::is_reflectable<T>();
uint64_t fields<T>() requires public_record<T> {
  return std::meta::type<T>().field_count();
}
int main() {
  static_assert(std::meta::is_bool<bool>());
  static_assert(!std::meta::is_bool<int>());
  std::atomic<bool> truth{true};
  std::atomic<int> number{7};
  return load<bool>(truth) && load_truth<bool>(truth) && load<int>(number) == 7 &&
         increment<int>(3) == 4 && fields<Record>() == uint64_t(1) ? 0 : 1;
}
)", true),
      "source scalar policies transport common proofs across alternatives");

  rejected(check(root, directory, "disjunction_does_not_invent_scalar", R"(
#include <std/atomic>
#include <std/meta>
concept integer_or_float<T> = std::meta::is_integral<T>() ||
                              std::meta::is_floating_point<T>();
T load<T>(std::atomic<T>& value) requires integer_or_float<T> {
  return value.load();
}
int main() { return 0; }
)", true),
           "one non-atomic alternative cannot justify atomic storage");

  accepted(check(root, directory, "source_storage_policy", R"(
#include <std/memory>
#include <std/meta>
struct Payload { int value = 0; };
struct Borrowed {
  int& value;
  Borrowed(int& source) : value(source) {}
};
concept complete_object<T> = std::meta::has_object_layout<T>();
concept independent<T> = complete_object<T> && std::meta::is_borrow_free<T>();
uint64_t bytes<T>() requires complete_object<T> { return std::layout_of<T>().bytes; }
unsafe void retire<T>(T* address) requires complete_object<T> {
  unsafe { std::destroy_at<T>(address); }
}
std::uninitialized<T> slot<T>() requires independent<T> && std::bitwise_relocatable<T> {
  return std::uninitialized<T>();
}
int main() {
  static_assert(std::meta::has_object_layout<Payload>());
  static_assert(std::meta::has_object_layout<Borrowed>());
  static_assert(!std::meta::has_object_layout<void>());
  static_assert(std::meta::is_borrow_free<Payload>());
  static_assert(!std::meta::is_borrow_free<Borrowed>());
  static_assert(!std::meta::is_borrow_free<int&>());
  static_assert(std::meta::is_borrow_free<int*>());
  auto vacant = slot<Payload>();
  return bytes<Payload>() > uint64_t(0) ? 0 : 1;
}
)"),
           "independent source object-layout and borrow-state policies");

  rejected(check(root, directory, "copying_does_not_prove_storage", R"(
#include <std/memory>
std::uninitialized<T> slot<T>() requires std::copyable<T> {
  return std::uninitialized<T>();
}
int main() { return 0; }
)"),
           "copyability alone does not establish independent storage");

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
)"),
           "heterogeneous operator result and grouped checks");

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
T standard_empty<T>() requires std::default_initializable<T> { return T(); }
class Leaf { public: Leaf() {} };
class Holder<T> {
  T value;
public:
  Holder() requires std::default_initializable<T> : value(T()) {}
};
int main() {
  int value = 9;
  Box result = make<Box, int>(value);
  Leaf leaf = standard_empty<Leaf>();
  Holder<Leaf> holder{};
  return result.read() == 9 && empty<int>() == 0 ? 0 : 1;
}
)"),
           "default and argument-bearing construction");

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
  static_assert(requires {
    { std::move(owner) } -> std::unique_ptr<int>;
  });
  constexpr bool can_move = requires(std::unique_ptr<int> input) {
    { std::move(input) } -> std::unique_ptr<int>;
  };
  return calls == 0 && can_move && *owner == 7 ? 0 : 1;
}
)"),
           "queries neither execute calls nor move caller objects");

  accepted(check(root, directory, "macro_query_domains", R"(
mut int observed = 0;
#macro inspect(bool query) {
  bool subject = false;
  if (query) { observed += 1; }
}
#macro nested(bool query) {
  bool subject = false;
  inspect(query);
}
int main() {
  int subject = 7;
  inspect(requires { { subject + subject } -> int; });
  inspect(requires(int& subject) { { subject + subject } -> int; });
  nested(requires { { subject + subject } -> int; });
  nested(requires(int& subject) { { subject + subject } -> int; });
  inspect(requires(int& value) {
    { (requires { { value + value } -> int; }) } -> bool;
  });
  inspect(requires {
    { ([]() -> int { int subject = 2; return subject; })() } -> int;
  });
  inspect(requires(int& value) {
    { ([&value]() -> int { return value; })() } -> int;
  });
  return 0;
}
)"),
           "macro arguments preserve caller domains and query-local bindings");

  rejected(
      check(root, directory, "macro_definition_query", R"(
#macro inspect(bool value) {
  if (requires(int& value) { { value + value } -> int; }) {}
}
int main() { inspect(true); return 0; }
)"),
      "typed macro definitions retain their documented query-body boundary",
      "GTI-S2099");

  accepted(
      check(root, directory, "access", R"(
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
)"),
      "private access, receiver mutability and explicit unsafe obligations");

  accepted(check(root, directory, "private_self_query", R"(
class Vault {
  int secret() { return 7; }
public:
  int probe() {
    static_assert(requires(Vault& other) { { (other.secret()) } -> int; });
    return this.secret();
  }
};
int main() { Vault value{}; return value.probe() - 7; }
)"),
           "direct queries keep ordinary private self-access");

  rejected(check(root, directory, "private_concept_scope", R"(
concept public_read<T> = requires(T& value) { { value.secret() } -> int; };
int select<T>() requires public_read<T> { return 1; }
class Vault {
  int secret() { return 7; }
public:
  int probe() { return select<Vault>(); }
};
int main() { Vault value{}; return value.probe(); }
)"),
           "public concepts cannot borrow a caller class's private privilege");

  accepted(
      check(root, directory, "predicates", R"(
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
)"),
      "reflection implication, disjunction and ordinary constexpr predicates");

  accepted(check(root, directory, "explicit_query_arguments", R"(
int number<T>() requires std::integral<T> { return 1; }
int combine<A, B>(int left, int right) { return left + right; }
struct Record<T> { int read() { return 1; } };
Record<T> make_record<T>() { return Record<T>(); }
concept produces_int<T> = requires {
  { number<T>() } -> int;
  { (number<T>()) + number<T>() } -> int;
  { -(number<T>()) } -> int;
  { combine<T, T>(number<T>(), number<T>()) } -> int;
  { make_record<T>().read() } -> int;
};
int use<T>() requires produces_int<T> { return 1; }
int direct<T>() {
  if constexpr (requires { { number<T>() } -> int; }) {
    return 1;
  } else {
    return 2;
  }
}
int main() {
  int local = 3;
  static_assert(requires { { local + local } -> int; });
  return use<int>() == 1 && direct<int>() == 1 && direct<bool>() == 2 &&
         local == 3 ? 0 : 1;
}
)"),
           "all explicit query type arguments bind in parser traversal order");

  accepted(check(root, directory, "generic_callable_query", R"(
concept invokes<F, A, R> = requires(F& function, A& argument) {
  { function(argument) } -> R;
};
R invoke<F, A, R>(F& function, A& argument) requires invokes<F, A, R> {
  return function(argument);
}
int invoke_int<F>(F& function, int& argument) requires invokes<F, int, int> {
  return function(argument);
}
struct AddOne {
  int operator()(int& value) { return value + 1; }
};
int main() {
  int value = 4;
  AddOne add_one{};
  auto lambda = [](int& input) -> int { return input + 2; };
  return invoke<AddOne, int, int>(add_one, value) == 5 &&
         invoke_int(lambda, value) == 6 ? 0 : 1;
}
)"),
           "call expression proofs support user functors and lambdas");

  rejected(check(root, directory, "callable_query_wrong_result", R"(
concept invokes<F, A, R> = requires(F& function, A& argument) {
  { function(argument) } -> R;
};
int invoke_int<F>(F& function, int& argument) requires invokes<F, int, int> {
  return function(argument);
}
struct WrongResult { bool operator()(int& value) { return value != 0; } };
int main() {
  int value = 4;
  WrongResult function{};
  return invoke_int(function, value);
}
)"),
           "call expression proofs retain exact result requirements");

  rejected(check(root, directory, "callable_query_unsafe", R"(
concept invokes<F, A, R> = requires(F& function, A& argument) {
  { function(argument) } -> R;
};
int invoke_int<F>(F& function, int& argument) requires invokes<F, int, int> {
  return function(argument);
}
struct UnsafeCall { unsafe int operator()(int& value) { return value; } };
int main() {
  int value = 4;
  UnsafeCall function{};
  return invoke_int(function, value);
}
)"),
           "safe callable proofs cannot hide an unsafe invocation");

  accepted(
      check(root, directory, "consuming_query_filter", R"(
struct Once { int operator()(int& value) && { return value; } };
int main() {
  static_assert(requires(Once function, int& argument) {
    { std::move(function)(argument) } -> int;
  });
  static_assert(!(requires(Once& function, int& argument) {
    { std::move(function)(argument) } -> int;
  }));
  return 0;
}
)"),
      "owned consuming queries do not authorize consumption through a borrow");

  rejected(check(root, directory, "consuming_query_borrow_proof", R"(
concept owns_invocation<F, A, R> = requires(F function, A& argument) {
  { std::move(function)(argument) } -> R;
};
int borrowed<F>(F& function, int& argument)
    requires owns_invocation<F, int, int> {
  return std::move(function)(argument);
}
struct Once { int operator()(int& value) && { return value; } };
int main() {
  Once function{};
  int argument = 3;
  return borrowed(function, argument);
}
)"),
           "an owned receiver filter cannot weaken borrowed movement checks",
           "GTI-S2018");

  accepted(
      check(root, directory, "declaration_query_scope", R"(
#include <std/meta>
namespace definition {
  using Chosen = int;
  int leaf<T>() requires std::integral<T> { return 1; }
  concept available<T> = requires { { leaf<Chosen>() } -> int; };
  constexpr bool arbitrary<T>() {
    return std::meta::is_integral<T>() && (1 + 1 == 2);
  }
  concept predicate<T> = arbitrary<Chosen>();
  int use<T>() requires available<T> && predicate<T> { return 1; }
}
int relay<Chosen>() {
  int leaf = 9;
  int arbitrary = 3;
  return definition::use<int>() + leaf + arbitrary - 12;
}
int main() { return relay<bool>() - 1; }
)"),
      "declaration queries cannot capture caller locals or generic shadows");

  rejected(check(root, directory, "explicit_query_rejection", R"(
int number<T>() requires std::integral<T> { return 1; }
concept produces_int<T> = requires { { number<T>() } -> int; };
int use<T>() requires produces_int<T> { return 1; }
int main() { return use<bool>(); }
)"),
           "explicit type arguments retain the selected callee's constraints");

  rejected(check(root, directory, "wrong_result", R"(
concept adds_bool<T> = requires(T& left, T& right) { { left + right } -> bool; };
int use<T>(T& value) requires adds_bool<T> { return 0; }
int main() { int value = 1; return use(value); }
)"),
           "exact result mismatch");

  const std::string multiOriginSource = R"(
concept readable_second<L, R> = requires(L& unused, R& receiver) {
  { receiver.read() } -> int;
};
int use<L, R>(L& left, R& right) requires readable_second<L, R> { return 0; }
struct Missing {};
int main() { int left = 1; Missing right{}; return use(left, right); }
)";
  const auto multiOrigin =
      check(root, directory, "expression_diagnostic_origin", multiOriginSource);
  const auto multiFailure = std::find_if(
      multiOrigin.diagnostics.begin(), multiOrigin.diagnostics.end(),
      [](const auto &item) { return item.code == "GTI-S2029"; });
  const auto writtenApplication = multiOriginSource.find(
      "readable_second<L, R>", multiOriginSource.find("int use"));
  expect(
      !multiOrigin.hirValid && multiFailure != multiOrigin.diagnostics.end() &&
          multiFailure->message == "Generic arguments do not satisfy concept "
                                   "'readable_second<int32_t, Missing>'." &&
          multiFailure->primary.start == multiOriginSource.rfind(");") &&
          std::any_of(multiFailure->related.begin(),
                      multiFailure->related.end(),
                      [&](const auto &note) {
                        return note.span.start == writtenApplication &&
                               note.message.find("Required application '") == 0;
                      }) &&
          std::any_of(multiFailure->related.begin(),
                      multiFailure->related.end(),
                      [](const auto &note) {
                        return note.message.find(
                                   "Unknown member 'read' on 'Missing'") !=
                               std::string::npos;
                      }),
      "multi-parameter expression failures name the concept without "
      "falsely blaming an unused first argument");

  const std::string unaryOriginSource = R"(
concept readable<T> = requires(T& receiver) { { (receiver.read()) } -> int; };
int use<T>(T& receiver) requires readable<T> { return 0; }
int main() { int value = 1; return use(value); }
)";
  const auto unaryOrigin =
      check(root, directory, "unary_expression_origin", unaryOriginSource);
  const auto unaryFailure = std::find_if(
      unaryOrigin.diagnostics.begin(), unaryOrigin.diagnostics.end(),
      [](const auto &item) { return item.code == "GTI-S2029"; });
  expect(
      !unaryOrigin.hirValid && unaryFailure != unaryOrigin.diagnostics.end() &&
          unaryFailure->primary.start == unaryOriginSource.rfind("value") &&
          unaryFailure->message.find("Argument 1 has type 'int32_t'") == 0 &&
          unaryFailure->message.find("readable<int32_t>") != std::string::npos,
      "an unambiguous unary receiver retains its actual call-argument span");

  const std::string independentResultSource = R"(
concept reads_as<T, R> = requires(T& receiver) { { (receiver.read()) } -> R; };
int use<T, R>(T& receiver, R& result) requires reads_as<T, R> { return 0; }
struct Reader { int read() { return 1; } };
int main() { Reader receiver{}; bool result = false; return use(receiver, result); }
)";
  const auto independentResult = check(
      root, directory, "expression_result_origin", independentResultSource);
  const auto independentFailure =
      std::find_if(independentResult.diagnostics.begin(),
                   independentResult.diagnostics.end(),
                   [](const auto &item) { return item.code == "GTI-S2029"; });
  expect(
      !independentResult.hirValid &&
          independentFailure != independentResult.diagnostics.end() &&
          independentFailure->primary.start ==
              independentResultSource.rfind(");") &&
          independentFailure->message == "Generic arguments do not satisfy "
                                         "concept 'reads_as<Reader, bool>'.",
      "an independent required result does not falsely blame a unary receiver");

  rejected(check(root, directory, "false_predicate", R"(
#include <std/meta>
concept integral_only<T> = std::meta::is_integral<T>();
int use<T>(T value) requires integral_only<T> { return 0; }
int main() { return use(true); }
)"),
           "false reflected predicate");

  rejected(check(root, directory, "nonconstant", R"(
bool runtime_only<T>() { return true; }
concept invalid<T> = runtime_only<T>();
int use<T>() requires invalid<T> { return 0; }
int main() { return use<int>(); }
)"),
           "non-constexpr predicate");

  rejected(check(root, directory, "not_boolean", R"(
constexpr int not_bool<T>() { return 1; }
concept invalid<T> = not_bool<T>();
int use<T>() requires invalid<T> { return 0; }
int main() { return use<int>(); }
)"),
           "predicate must have exact bool type");

  rejected(check(root, directory, "alternative", R"(
concept first<T> = requires(T& value) { { value + value } -> int; };
concept second<T> = requires(T& value) { { value - value } -> bool; };
int broken<T>(T& value) requires first<T> || second<T> {
  return value + value;
}
int main() { int value = 1; return broken(value); }
)"),
           "alternative requirements cannot invent a shared operation",
           "GTI-S2004");

  accepted(check(root, directory, "compound_type_arguments", R"(
#include <std/meta>
bool flag<T>() { return std::meta::is_integral<T>(); }
int number<T>() { return 2; }
concept selects<T> = requires {
  { flag<T>() && flag<T>() } -> bool;
  { flag<T>() ? number<T>() : number<T>() } -> int;
};
int selected<T>() requires selects<T> { return 0; }
int main() { return selected<int>(); }
)"),
           "compound requirements retain explicit generic arguments");

  accepted(check(root, directory, "nested_unsafe_permission", R"(
concept raw_read<T> = requires(T& value) {
  { unsafe { value.raw() } } -> int;
};
struct Device { unsafe int raw() { return 9; } };
int read<T>(T& value) requires raw_read<T> { return unsafe { value.raw() }; }
int main() { Device value{}; return read(value) == 9 ? 0 : 1; }
)"),
           "unsafe expressions retain their call obligation in generic facts");
  rejected(check(root, directory, "nested_unsafe_permission_missing", R"(
concept raw_read<T> = requires(T& value) {
  { unsafe { value.raw() } } -> int;
};
int read<T>(T& value) requires raw_read<T> { return value.raw(); }
)"),
           "a query cannot hide an unsafe obligation", "GTI-S2055");

  const std::string formatting =
      R"(concept sum<T>=requires(T& a,T& b){{a+b}->T;{a==b}->bool;};
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
