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
static_assert(!lang::compilerStructuralConstraint("bidirectional_iterator"));
static_assert(!lang::compilerStructuralConstraint("random_access_iterator"));
static_assert(!lang::compilerStructuralConstraint("once_action"));
static_assert(!lang::compilerStructuralConstraint("writes_to"));
static_assert(!lang::compilerStructuralConstraint("subscripts_to_mut"));
static_assert(!lang::compilerStructuralConstraint("sentinel_for"));
static_assert(!lang::compilerStructuralConstraint("sized_sentinel_for"));
static_assert(!lang::compilerStructuralConstraint("indirect_predicate"));
static_assert(!lang::compilerStructuralConstraint("indirect_operation"));
static_assert(!lang::compilerStructuralConstraint("range_predicate"));
static_assert(!lang::compilerStructuralConstraint("range_operation"));
static_assert(!lang::compilerStructuralConstraint("input_range"));
static_assert(!lang::compilerStructuralConstraint("range_of"));
static_assert(!lang::compilerStructuralConstraint("mutable_range_of"));
static_assert(!lang::compilerStructuralConstraint("input_iterator"));
static_assert(!lang::compilerStructuralConstraint("dereferences_to"));
static_assert(!lang::compilerStructuralConstraint("writable_iterator"));
static_assert(!lang::compilerStructuralConstraint("sized_range"));
static_assert(lang::compilerConstraints.empty());
static_assert(lang::compilerStructuralConstraints.empty());
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
static_assert(!lang::compilerConstraint("equality_comparable"));
static_assert(!lang::compilerConstraint("relationally_ordered"));

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
    for (const auto &item : result.diagnostics) {
      std::cerr << "  " << item.code << " at " << item.primary.line << ": "
                << item.message << '\n';
      for (const auto &note : item.related)
        std::cerr << "    " << note.message << '\n';
    }
}

void rejected(const lang::FrontendResult &result, const std::string &name,
              const std::string &code = "GTI-S2029") {
  const auto diagnostic =
      std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
                   [&](const auto &item) { return item.code == code; });
  expect(diagnostic != result.diagnostics.end(), name + " diagnostic");
  if (diagnostic == result.diagnostics.end())
    for (const auto &item : result.diagnostics)
      std::cerr << "  " << item.code << ": " << item.message << '\n';
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
      check(root, directory, "inferred_requirement_results", R"GTI(
concept observes<I> = requires(I& cursor) { { *cursor }; };
concept edits<I> = requires(mut I& cursor) {
  { *cursor } -> mut auto& [[borrowed_from(cursor)]];
};
struct Cell {
  mut int value = 1;
  int& operator*() { return this.value; }
  mut int& operator*() mut { return this.value; }
};
struct Value { int operator*() { return 2; } };
struct Empty {};
T read<I,T>(I& cursor) requires observes<I> { return *cursor; }
void write<I,T>(mut I& cursor, T value) requires edits<I> { *cursor = value; }
bool same<I>(I& cursor, int value) requires observes<I> { return *cursor == value; }
void exact<I>(mut I& cursor) requires edits<I> && requires(mut I& input) {
  { *input } -> mut int& [[borrowed_from(input)]];
} { *cursor = 3; }
int main() {
  static_assert(requires(Cell& cursor) { { *cursor }; });
  static_assert(requires(Value& cursor) { { *cursor }; });
  static_assert(!(requires(Empty& cursor) { { *cursor }; }));
  static_assert(!(requires(mut Value& cursor) {
    { *cursor } -> mut auto& [[borrowed_from(cursor)]];
  }));
  static_assert(!(requires(Cell& cursor) {
    { *cursor } -> mut auto& [[borrowed_from(cursor)]];
  }));
  static_assert(!(requires(Cell& left, Cell& right) {
    { *right } -> auto& [[borrowed_from(left)]];
  }));
  mut Cell cell{};
  Value value{};
  write(cell, 3);
  exact(cell);
  if (!same(cell, 3)) { return 9; }
  return read<Cell,int>(cell) + read<Value,int>(value) - 5;
}
)GTI"),
      "inferred results preserve existence, access and exact borrowed origins");

  rejected(check(root, directory, "inferred_comparison_rechecked", R"GTI(
concept observes<I> = requires(I& cursor) { { *cursor }; };
struct Uncomparable {};
struct Reader { Uncomparable operator*() { return Uncomparable(); } };
bool same<I>(I& cursor, int value) requires observes<I> { return *cursor == value; }
int main() { Reader reader{}; return same(reader, 2) ? 0 : 1; }
)GTI"),
           "a deferred comparison must resolve in the concrete instance",
           "GTI-S2022");

  accepted(
      check(root, directory, "iteration_requirements", R"GTI(
#include <std/vector>
concept readable<R> = requires(R& range) { for (auto& element : range) {} };
concept ints<R> = requires(R& range) { for (int& element : range) {} };
concept editable<R> = requires(mut R& range) { for (mut int& element : range) {} };
int sum<R>(R& range) requires ints<R> {
  mut int total = 0;
  for (int& element : range) { total += element; }
  return total;
}
void clear<R>(mut R& range) requires editable<R> {
  for (mut int& element : range) { element = 0; }
}
struct Empty {};
int main() {
  static_assert(requires(int[3]& range) { for (auto& element : range) {} });
  static_assert(requires(mut int[3]& range) { for (mut int& element : range) {} });
  static_assert(!(requires(int[3]& range) { for (mut int& element : range) {} }));
  static_assert(!(requires(int[3]& range) { for (bool& element : range) {} }));
  static_assert(!(requires(Empty& range) { for (auto& element : range) {} }));
  static_assert(requires(std::vector<int>& range) { for (auto& element : range) {} });
  mut std::vector<int> values{};
  values.push_back(3);
  int result = sum(values);
  clear(values);
  return result == 3 ? 0 : 1;
}
)GTI"),
      "iteration requirements reuse loop analysis and export generic proofs");

  accepted(check(root, directory, "source_sized_range_policy", R"GTI(
#include <std/iterator>
#include <std/vector>
std::size_t measured<R>(R& range) requires std::sized_range<R> {
  return range.size();
}
struct Cursor {
  int value = 0;
  int& operator*() { return this.value; }
  void operator++() mut {}
  bool operator!=(Cursor& other) { return false; }
};
struct WrongSize {
  Cursor begin() { return Cursor(); }
  Cursor end() { return Cursor(); }
  int size() { return 1; }
};
struct MutableSize {
  Cursor begin() { return Cursor(); }
  Cursor end() { return Cursor(); }
  std::size_t size() mut { return std::size_t(1); }
};
int main() {
  static_assert(!(requires(WrongSize& range) { { measured(range) } -> std::size_t; }));
  static_assert(!(requires(MutableSize& range) { { measured(range) } -> std::size_t; }));
  int[3] fixed = {1, 2, 3};
  std::vector<int> dynamic{};
  return measured(fixed) == std::size_t(3) && measured(dynamic) == std::size_t(0) ? 0 : 1;
}
)GTI"),
           "source sized_range preserves checked fixed-array size and exact "
           "read-only results");

  accepted(
      check(root, directory, "dependent_borrowed_query_result", R"GTI(
#include <std/meta>
struct Reader { int value = 1; int& operator*() { return this.value; } };
constexpr bool result_matches<I, R>(std::meta::type<I> owner, std::meta::type<R> result) {
  if constexpr (std::meta::type<R>().is_reference()) {
    return requires(I& input) { { *input } -> R [[borrowed_from(input)]]; };
  } else { return requires(I& input) { { *input } -> R; }; }
}
constexpr bool inspect<I>() {
  mut bool good = false;
  constexpr for (auto op : std::meta::lookup_operators_of<I>()) {
    if constexpr (op.name() == "operator*") {
      good = result_matches(std::meta::type<I>(), op.result());
    }
  }
  return good;
}
int main() { static_assert(inspect<Reader>()); return 0; }
)GTI"),
      "dependent borrowed result attributes are distinct from array suffixes");

  accepted(check(root, directory, "source_range_callable_policy", R"GTI(
#include <std/functional>
#include <std/algorithm>
#include <std/meta>
struct End {};
struct Cursor {
  int value = 1;
  int& operator*() { return this.value; }
  void operator++() mut {}
  bool operator!=(End& end) { return false; }
};
struct Range {
  Cursor begin() { return Cursor(); }
  End end() { return End(); }
  bool operator[](std::size_t index) { return false; }
};
struct WantsInt { bool operator()(int& value) { return value > 0; } };
struct WantsBool { bool operator()(bool& value) { return value; } };
struct Consumes { bool operator()(int& value) && { return true; } };
int main() {
  static_assert(std::meta::is_array<int[3]>());
  static_assert(std::meta::type<int[0]>().is_array());
  static_assert(!std::meta::is_array<Range>());
  static_assert(requires(Range& range) { { std::all_of(range, WantsInt()) } -> bool; });
  static_assert(!(requires(Range& range) { { std::all_of(range, WantsBool()) } -> bool; }));
  static_assert(!(requires(Range& range) { { std::all_of(range, Consumes()) } -> bool; }));
  int[3] numbers = {1, 2, 3};
  return std::all_of(numbers, WantsInt()) ? 0 : 1;
}
)GTI"),
           "source range callable queries inspect the actual element, not "
           "unrelated indexing");

  rejected(check(root, directory, "overload_predicate_diagnostic", R"GTI(
constexpr bool available<T>() { return false; }
[[unsatisfied("Use a type accepted by the source policy.")]]
concept admitted<T> = available<T>();
int pick<T>(T value) requires admitted<T> { return 1; }
int pick<T>(T value, int extra) { return extra; }
int main() { return pick(3); }
)GTI"),
           "overload filtering retains ordinary source-predicate failures",
           "GTI-S2029");

  accepted(check(root, directory, "source_sentinel_policy", R"GTI(
#include <std/iterator>
struct End { int position = 8; };
struct Cursor {
  int position = 3;
  bool operator!=(End& end) { return this.position != end.position; }
  std::ptrdiff_t operator-(End& end) { return std::ptrdiff_t(this.position - end.position); }
};
struct CopiedEnd {
  bool operator!=(End end) { return true; }
  std::ptrdiff_t operator-(End end) { return std::ptrdiff_t(-1); }
};
struct Ambiguous {
  bool operator!=(End& end) { return true; }
  bool operator!=(End end) { return true; }
};
struct WrongDistance {
  bool operator!=(End& end) { return true; }
  uint64_t operator-(End& end) { return uint64_t(1); }
};
class Hidden { bool operator!=(End& end) { return true; } };
bool differs<I, S>(I& position, S& end) requires std::sentinel_for<S, I> {
  return position != end;
}
std::ptrdiff_t offset<I, S>(I& position, S& end) requires std::sized_sentinel_for<S, I> {
  return position - end;
}
int main() {
  static_assert(requires(Cursor& p, End& e) { { differs(p, e) } -> bool; });
  static_assert(requires(Cursor& p, End& e) { { offset(p, e) } -> std::ptrdiff_t; });
  static_assert(requires(CopiedEnd& p, End& e) { { offset(p, e) } -> std::ptrdiff_t; });
  static_assert(!(requires(Ambiguous& p, End& e) { { differs(p, e) } -> bool; }));
  static_assert(!(requires(WrongDistance& p, End& e) { { offset(p, e) } -> std::ptrdiff_t; }));
  static_assert(!(requires(Hidden& p, End& e) { { differs(p, e) } -> bool; }));
  static_assert(!(requires(int& p, int& e) { { differs(p, e) } -> bool; }));
  Cursor position{}; End end{};
  return differs(position, end) && offset(position, end) == std::ptrdiff_t(-5) ? 0 : 1;
}
)GTI"),
           "sentinel policies are source-defined exact safe expression "
           "requirements");

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
)",
            true),
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
)",
                 true),
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
  static_assert(std::meta::is_raw_pointer<int*>());
  static_assert(std::meta::type<void*>().is_raw_pointer());
  static_assert(!std::meta::is_raw_pointer<int>());
  static_assert(!std::meta::is_raw_pointer<int&>());
  static_assert(!std::meta::is_raw_pointer<std::unique_ptr<int>>());
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

  accepted(
      check(root, directory, "inline_source_requirements", R"(
concept reads<T> = requires(T& value) { { value.read() } -> int; };
concept sums<T> = requires(T& left, T& right) { { left + right } -> int; };
struct Counter {
  int value = 4;
  int read() { return this.value; }
  int operator+(Counter& other) { return this.value + other.value; }
};
int read<reads T>(T& value) { return value.read(); }
int sum<sums T = Counter>(T& left, T& right) { return left + right; }
class Reader {
public:
  int read<reads T>(T& value) { return value.read(); }
};
int main() {
  Counter a{}; Counter b{}; Reader reader{};
  return read(a) + sum(a, b) + reader.read(a) == 16 ? 0 : 1;
}
)"),
      "inline source concepts preserve full signature and operator proofs");

  rejected(check(root, directory, "inline_source_requirement_rejection", R"(
concept reads<T> = requires(T& value) { { value.read() } -> int; };
struct Wrong { bool read() { return true; } };
int read<reads T>(T& value) { return value.read(); }
int main() { Wrong value; return read(value); }
)"),
           "inline source concepts validate the full concrete requirement");

  accepted(check(root, directory, "source_comparison_policy", R"(
#include <std/meta>
struct Key {
  int value = 0;
  bool operator==(Key& other) { return this.value == other.value; }
  bool operator!=(Key& other) { return this.value != other.value; }
  bool operator<(Key& other) { return this.value < other.value; }
  bool operator<=(Key& other) { return this.value <= other.value; }
  bool operator>(Key& other) { return this.value > other.value; }
  bool operator>=(Key& other) { return this.value >= other.value; }
};
bool same<std::equality_comparable T>(T& a, T& b) { return a == b; }
bool before<std::totally_ordered T>(T& a, T& b) { return a < b; }
concept ordered_alias<T> = std::totally_ordered<T>;
bool aliased_before<T>(T& a, T& b) requires ordered_alias<T> {
  return before(a, b);
}
bool ordered_same<std::totally_ordered T>(T& a, T& b) { return same(a, b); }
bool integer_before<T>(T& a, T& b) requires std::meta::is_integral<T>() {
  return before(a, b);
}
struct Contracts {
  int read() { return 0; }
  unsafe int raw() { return 0; }
  int operator()() && { return 0; }
  int checked<T>(T& value) requires std::integral<T> { return 0; }
};
int main() {
  static_assert(!std::meta::method<Contracts, 0>().is_consuming());
  static_assert(!std::meta::method<Contracts, 0>().is_unsafe());
  static_assert(std::meta::method<Contracts, 1>().is_unsafe());
  static_assert(std::meta::method<Contracts, 2>().is_consuming());
  static_assert(std::meta::method<Contracts, 3>().is_generic());
  static_assert(std::meta::method<Contracts, 3>().is_constrained());
  static_assert(std::detail::read_equality_members<Key>());
  static_assert(std::detail::read_ordering_members<Key>());
  Key a{.value = 1}; Key b{.value = 2};
  int x = 1; int y = 2;
  return !same(a, b) && !ordered_same(a, b) && before(a, b) &&
         aliased_before(a, b) && integer_before(x, y) ? 0 : 1;
}
)"),
           "source comparison policy uses general declaration facts and "
           "operation proofs");

  rejected(check(root, directory, "source_comparison_policy_exactness", R"(
struct ValueParameter {
  bool operator==(ValueParameter other) { return true; }
  bool operator!=(ValueParameter other) { return false; }
};
bool same<std::equality_comparable T>(T& a, T& b) { return a == b; }
int main() { ValueParameter value{}; return same(value, value) ? 0 : 1; }
)"),
           "source comparison policy retains its exact read-reference "
           "parameter contract");

  rejected(check(root, directory, "unproven_comparison_policy", R"(
bool before<std::totally_ordered T>(T& a, T& b) { return a < b; }
bool missing_proof<T>(T& a, T& b) { return before(a, b); }
int main() { return 0; }
)"),
           "isolated expression queries cannot assume their own comparison "
           "capability");

  accepted(check(root, directory, "comparison_policy_boundaries", R"(
class Hidden {
  bool operator==(Hidden& other) { return true; }
  bool operator!=(Hidden& other) { return false; }
};
bool same<std::equality_comparable T>(T& a, T& b) { return a == b; }
int main() {
  static_assert(!(requires(Hidden& a, Hidden& b) { { same(a, b) } -> bool; }));
  static_assert(requires(bool& a, bool& b) { { same(a, b) } -> bool; });
  static_assert(requires(int*& a, int*& b) { { same(a, b) } -> bool; });
  return 0;
}
)"),
           "comparison source policy preserves private access without "
           "excluding primitive equality");

  accepted(check(root, directory, "source_mutation_requirements", R"(
#include <std/iterator>
#include <std/meta>
concept adjustable<T> = requires(mut T& value, int amount) {
  { ++value } -> void;
  { --value } -> void;
  { value += amount } -> void;
  { value -= amount } -> void;
};
struct Counter {
  mut int value = 0;
  void operator++() mut { this.value += 1; }
  void operator--() mut { this.value -= 1; }
  void operator+=(int amount) mut { this.value += amount; }
  void operator-=(int amount) mut { this.value -= amount; }
};
struct Inherited : public Counter {};
struct CursorBase<T> {
  mut int position = 0;
  int operator*() { return this.position; }
  void operator++() mut { this.position += 1; }
  void operator--() mut { this.position -= 1; }
  void operator+=(std::ptrdiff_t amount) mut { this.position += int(amount); }
  void operator-=(std::ptrdiff_t amount) mut { this.position -= int(amount); }
};
struct Cursor : public CursorBase<int> {};
void jump<std::random_access_cursor I>(mut I& iterator, std::ptrdiff_t amount) {
  iterator += amount;
  --iterator;
}
void adjust<adjustable T>(mut T& value, int amount) {
  ++value;
  value += amount;
  --value;
  value -= amount;
}
int main() {
  mut Counter value{};
  adjust(value, 7);
  mut Inherited inherited{};
  adjust(inherited, 2);
  mut Cursor cursor{};
  jump(cursor, std::ptrdiff_t(4));
  static_assert(std::meta::type<Cursor>().method_count() == uint64_t(0));
  return value.value == 0 && inherited.value == 0 && cursor.position == 3 ? 0 : 1;
}
)"),
           "general mutation requirements prove operations on ordinary "
           "source classes without an iterator capability");

  accepted(check(root, directory, "source_once_action", R"(
#include <std/functional>
concept task<F> = requires(F action) { { std::move(action)() } -> void; };
void run<F>(F action) requires task<F> { std::move(action)(); }
void forward<F>(F action) requires std::once_action<F> {
  std::invoke_once(std::move(action));
}
struct Once { void operator()() && {} };
struct Read { void operator()() {} };
struct Mutable { void operator()() mut {} };
struct Wrong { int operator()() && { return 1; } };
struct NeedsArgument { void operator()(int value) {} };
unsafe void unsafe_action() {}
class Hidden { void operator()() {} };
int main() {
  run(Once());
  forward(Once());
  forward(Read());
  forward(Mutable());
  forward([]() -> void {});
  static_assert(!(requires(Wrong action) { { std::move(action)() } -> void; }));
  static_assert(!(requires(NeedsArgument action) { { std::move(action)() } -> void; }));
  static_assert(!(requires { { unsafe_action() } -> void; }));
  static_assert(!(requires(Hidden action) { { std::move(action)() } -> void; }));
  return 0;
}
)"),
           "source consuming-call requirements preserve ordinary access, "
           "safety, arity and exact result checks");

  rejected(check(root, directory, "source_once_action_rejects_arity", R"(
#include <std/functional>
struct NeedsArgument { void operator()(int value) {} };
int main() { std::invoke_once(NeedsArgument()); return 0; }
)"),
           "source once_action does not invent a missing argument");

  rejected(check(root, directory, "source_once_action_rejects_result", R"(
#include <std/functional>
struct Wrong { int operator()() && { return 1; } };
int main() { std::invoke_once(Wrong()); return 0; }
)"),
           "source once_action requires exactly void");

  accepted(check(root, directory, "lookup_reflection_overrides", R"(
#include <std/meta>
class Parent<T> {
public:
  virtual int sample() { return 1; }
  T identity(T& value) { return value; }
};
class Child : public Parent<int> {
public:
  int sample() override { return 2; }
};
constexpr bool exact_lookup<T>() {
  mut int samples = 0;
  mut int identities = 0;
  mut bool exact = true;
  constexpr for (auto method : std::meta::lookup_methods_of<T>()) {
    if constexpr (method.name() == "sample") {
      samples += 1;
      if constexpr (!method.result().is_same<int>()) { exact = false; }
    }
    if constexpr (method.name() == "identity") {
      identities += 1;
      constexpr for (auto parameter : method.parameters()) {
        if constexpr (!parameter.type().is_same<int&>()) { exact = false; }
      }
    }
  }
  return exact && samples == 1 && identities == 1;
}
struct Tag<T> {
  constexpr Tag() {}
  constexpr int id() { return 7; }
};
constexpr int type_only<T>() {
  Tag<T> tag{};
  return tag.id();
}
int main() {
  static_assert(exact_lookup<Child>());
  static_assert(type_only<Child>() == 7);
  // Exact descriptor identity also proves that the overridden Parent::sample
  // is absent, and that the inherited identity owner retains its int argument.
  static_assert(std::meta::is_same<std::meta::lookup_method_list<Child>,
      std::meta::list<std::meta::method<Parent<int>, 1>,
                      std::meta::method<Child, 0>>>());
  static_assert(std::meta::is_same<std::meta::parameter_type<Parent<int>, 1, 0>, int&>());
  return 0;
}
)"),
           "lookup metadata substitutes inherited owners and excludes "
           "overridden declarations without changing declaration reflection");

  rejected(check(root, directory, "type_only_is_not_object_permission", R"(
class Virtual {
public:
  constexpr Virtual() {}
  virtual int value() { return 7; }
};
struct Holder<T> {
  T value;
  constexpr Holder() : value(T()) {}
};
constexpr int construct() { Holder<Virtual> holder{}; return 1; }
int main() { static_assert(construct() == 1); return 0; }
)"),
           "type-only constant arguments do not admit polymorphic objects",
           "GTI-S2057");

  accepted(
      check(root, directory, "borrowed_expression_requirements", R"(
struct Box {
  mut int value = 7;
  int& read() { return this.value; }
  mut int& edit() mut { return this.value; }
  int& operator*() { return this.value; }
  mut int& operator[](int index) mut { return this.value; }
};
[[borrowed_from(left)]] int& select(int& left, int& right) { return left; }
concept readable<C, T> = requires(C& owner) {
  { *owner } -> T& [[borrowed_from(owner)]];
};
concept editable<C, T> = requires(mut C& owner, int offset) {
  { owner.edit() } -> mut T& [[borrowed_from(owner)]];
  { owner[offset] } -> mut T& [[borrowed_from(owner)]];
};
[[borrowed_from(owner)]] T& peek<C, T>(C& owner) requires readable<C, T> {
  return *owner;
}
[[borrowed_from(owner)]] mut T& edit<C, T>(mut C& owner) requires editable<C, T> {
  return owner.edit();
}
void store<C, T>(mut C& owner, T value) requires editable<C, T> {
  mut T& slot = owner[0];
  slot = value;
}
int main() {
  mut Box box{};
  store<Box, int>(box, 9);
  if (peek<Box, int>(box) != 9) { return 1; }
  {
    mut int& value = edit<Box, int>(box);
    value = 11;
  }
  if (peek<Box, int>(box) != 11) { return 2; }
  static_assert(requires(Box& box) {
    { box.read() } -> int& [[borrowed_from(box)]];
    { *box } -> int& [[borrowed_from(box)]];
  });
  static_assert(requires(mut Box& box, int index) {
    { box.edit() } -> mut int& [[borrowed_from(box)]];
    { box[index] } -> mut int& [[borrowed_from(box)]];
  });
  static_assert(requires(int& left, int& right) {
    { select(left, right) } -> int& [[borrowed_from(left)]];
  });
  static_assert(!(requires(int& left, int& right) {
    { select(left, right) } -> int& [[borrowed_from(right)]];
  }));
  static_assert(!(requires(Box& box, Box& other) {
    { box.read() } -> int& [[borrowed_from(other)]];
  }));
  static_assert(!(requires(Box box) {
    { box.read() } -> int& [[borrowed_from(box)]];
  }));
  static_assert(!(requires(Box& box) {
    { box.edit() } -> mut int& [[borrowed_from(box)]];
  }));
  static_assert(!(requires(Box& box) {
    { box.read() } -> mut int& [[borrowed_from(box)]];
  }));
  static_assert(!(requires(Box& box) { { box.read() } -> int&; }));
  return 0;
}
)"),
      "checked borrowed queries preserve exact owner, access and referent");

  accepted(
      check(root, directory, "source_mutable_cursor_policy", R"GTI(
#include <std/iterator>
#include <std/meta>
struct Cursor {
  mut int value = 0;
  mut int& operator*() mut { return this.value; }
  mut int& operator[](std::ptrdiff_t offset) mut { return this.value; }
  int operator()() { return this.value; }
};
constexpr bool paired_operator_names() {
  mut bool index = false;
  mut bool call = false;
  constexpr for (auto member : std::meta::operators_of<Cursor>()) {
    if constexpr (member.name() == "operator[]") { index = true; }
    if constexpr (member.name() == "operator()") { call = true; }
  }
  return index && call;
}
void store<I>(mut I& cursor, int value) requires std::writes_to<I, int> {
  *cursor = value;
}
void store_at<I>(mut I& cursor, std::ptrdiff_t offset, int value)
    requires std::subscripts_to_mut<I, int> {
  cursor[offset] = value;
}
int main() {
  static_assert(paired_operator_names());
  mut Cursor cursor{};
  store(cursor, 9);
  store_at(cursor, std::ptrdiff_t(0), 11);
  return cursor.value == 11 ? 0 : 1;
}
)GTI"),
      "source cursor policies use general owner-aware reference requirements");

  accepted(check(root, directory, "source_erased_owner_policy", R"(
#include <std/meta>
#include <std/polymorphic>
#include <std/string>
interface Measure { int read(); };
class Point : public Measure {
public:
  Point() {}
  int read() override { return 7; }
};
class Noisy : public Measure {
public:
  Noisy() {}
  ~Noisy() {}
  int read() override { return 8; }
};
struct Owning { std::string text; };
int main() {
  static_assert(std::meta::is_polymorphic<Measure>());
  static_assert(std::meta::type<Point>().is_polymorphic());
  static_assert(!std::meta::is_polymorphic<int>());
  static_assert(std::meta::is_public_base_of<Measure, Point>());
  static_assert(std::meta::type<Point>().has_public_base<Measure>());
  static_assert(!std::meta::is_public_base_of<Point, Measure>());
  static_assert(!std::meta::is_public_base_of<Point, Point>());
  static_assert(!std::meta::is_public_base_of<int, Point>());
  static_assert(!std::meta::has_active_cleanup<Point>());
  static_assert(!std::meta::has_active_cleanup<int>());
  static_assert(std::meta::has_active_cleanup<Noisy>());
  static_assert(std::meta::type<Owning>().has_active_cleanup());
  static_assert(requires(std::detail::inline_payload_probe<Point, Measure>& p) {
    { p.matches() } -> bool;
  });
  static_assert(!(requires(std::detail::inline_payload_probe<Noisy, Measure>& p) {
    { p.matches() } -> bool;
  }));
  return 0;
}
)"),
           "source policy composes independent cleanup and public-base facts");

  accepted(check(root, directory, "source_cursor_policy_implication", R"(
#include <std/iterator>
void retreat<I>(mut I& cursor) requires std::bidirectional_cursor<I> {
  --cursor;
}
void jump_then_retreat<I>(mut I& cursor)
    requires std::random_access_cursor<I> && std::writes_to<I, int> {
  cursor += std::ptrdiff_t(2);
  retreat(cursor);
  std::advance(cursor, std::ptrdiff_t(-1));
}
int main() { return 0; }
)"),
           "nested conjunctions preserve opaque source predicate proofs");

  rejected(check(root, directory, "source_mutable_cursor_exact_index", R"(
#include <std/iterator>
struct Cursor {
  mut int value = 0;
  mut int& operator[](std::ptrdiff_t& offset) mut { return this.value; }
};
void require_cursor<I>() requires std::subscripts_to_mut<I, int> {}
int main() { require_cursor<Cursor>(); return 0; }
)"),
           "source index policy retains the exact by-value offset signature");

  const auto unknownOrigin =
      check(root, directory, "unknown_query_borrow_origin", R"(
int main() {
  static_assert(requires(int& value) {
    { value } -> int& [[borrowed_from(other)]];
  });
  return 0;
}
)");
  expect(!unknownOrigin.syntaxValid && !unknownOrigin.canGenerateCode() &&
             std::any_of(unknownOrigin.diagnostics.begin(),
                         unknownOrigin.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-P0001";
                         }),
         "borrow origin must resolve in its query (recovered HIR is not "
         "executable)");

  rejected(check(root, directory, "borrow_contract_is_not_shared_mutation", R"(
concept editable<C, T> = requires(mut C& owner) {
  { owner.edit() } -> mut T& [[borrowed_from(owner)]];
};
void broken<C, T>(C& owner, T value) requires editable<C, T> {
  mut T& slot = owner.edit();
  slot = value;
}
int main() { return 0; }
)"),
           "a mutable result proof does not grant mutable receiver access");

  rejected(check(root, directory, "mutation_requires_mutable_receiver", R"(
concept adjustable<T> = requires(mut T& value, int amount) {
  { value += amount } -> void;
};
void adjust<adjustable T>(T& value, int amount) { value += amount; }
int main() { return 0; }
)"),
           "a mutation proof never grants mutable access to a shared receiver",
           "GTI-S2029");

  rejected(check(root, directory, "borrowed_query_local_escape", R"(
concept readable<C> = requires(C& owner) {
  { owner.read() } -> int& [[borrowed_from(owner)]];
};
[[borrowed_from(input)]] int& escape<C>(C& input)
    requires readable<C> && std::default_initializable<C> {
  C local = C();
  return local.read();
}
int main() { return 0; }
)"),
           "a generic borrowed result cannot escape its local owner",
           "GTI-S2017");

  rejected(check(root, directory, "borrowed_query_live_alias", R"(
concept editable<C> = requires(mut C& owner) {
  { owner.edit() } -> mut int& [[borrowed_from(owner)]];
};
void alias<C>(mut C& owner) requires editable<C> {
  mut int& first = owner.edit();
  mut int& second = owner.edit();
  first = 1;
  second = 2;
}
int main() { return 0; }
)"),
           "generic result origins retain exclusive-loan conflicts",
           "GTI-S2017");

  rejected(check(root, directory, "borrowed_query_disjoint_origins", R"(
concept from_receiver<C> = requires(C& owner, int& input) {
  { owner.select(input) } -> int& [[borrowed_from(owner)]];
};
concept from_argument<C> = requires(C& owner, int& input) {
  { owner.select(input) } -> int& [[borrowed_from(input)]];
};
[[borrowed_from(owner)]] int& select<C>(C& owner, int& input)
    requires from_receiver<C> || from_argument<C> {
  return owner.select(input);
}
int main() { return 0; }
)"),
           "alternative result origins cannot invent a common owner",
           "GTI-S2000");

  accepted(check(root, directory, "borrowed_call_is_not_owned_result", R"(
[[borrowed_from(value)]] int& reference(int& value) { return value; }
class Borrower {
  int value = 1;
public:
  [[borrowed_from(input)]] int& operator()(int& input) { return input; }
  int& operator*() { return this.value; }
  int& operator[](int index) { return this.value; }
};
int main() {
  static_assert(!(requires(int& input) { { reference(input) } -> int; }));
  static_assert(!(requires(Borrower& f, int& input) { { f(input) } -> int; }));
  static_assert(!(requires(Borrower& f, int& input) { { (f(input)) } -> int; }));
  static_assert(!(requires(Borrower& f) { { *f } -> int; }));
  static_assert(!(requires(Borrower& f, int index) { { f[index] } -> int; }));
  static_assert(requires(Borrower& f, int& input) {
    { f(input) } -> int& [[borrowed_from(input)]];
  });
  static_assert(requires(Borrower& f) { { *f } -> int& [[borrowed_from(f)]]; });
  return 0;
}
)"),
           "owned-result queries cannot erase selected call/operator borrows");

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
concept editable<T>=requires(mut T& owner){{owner.edit()}->mut int& [[borrowed_from(owner)]];};
concept iterable<R>=requires(R& range){for(auto& element:range){}};
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
