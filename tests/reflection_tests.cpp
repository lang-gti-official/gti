// Static semantics of `constexpr for` and <std/meta>: expansion, deferral,
// diagnostics, access policy and the formatter. Runtime behaviour and the
// absence of reflection residue in emitted C++ are covered by
// reflection_runtime_test.py.
#include "gti/formatter.h"
#include "gti/frontend.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

int failures = 0;

void expect(bool valid, const std::string &message) {
  if (!valid) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult
analyze(const std::filesystem::path &root, const std::filesystem::path &entry,
        lang::FrontendPhase stopAfter = lang::FrontendPhase::Hir,
        lang::TrustedBodyAnalysis trustedBodies =
            lang::TrustedBodyAnalysis::Complete) {
  lang::FrontendOptions options;
  options.stopAfter = stopAfter;
  options.analyzeRecoveredProgram = true;
  options.trustedBodies = trustedBodies;
  return lang::Frontend(options).analyze(entry, std::nullopt,
                                         {root / "stdlib/prelude.gti"}, {},
                                         {root / "stdlib"});
}

lang::FrontendResult analyzeSource(const std::filesystem::path &root,
                                   const std::filesystem::path &directory,
                                   const std::string &name,
                                   const std::string &source,
                                   lang::TrustedBodyAnalysis trustedBodies =
                                       lang::TrustedBodyAnalysis::Complete) {
  const std::filesystem::path path = directory / name;
  std::ofstream(path) << source;
  return analyze(root, path, lang::FrontendPhase::Hir, trustedBodies);
}

bool hasCode(const lang::FrontendResult &result, std::string_view code) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.code == code;
                     });
}

std::size_t countCode(const lang::FrontendResult &result,
                      std::string_view code) {
  return static_cast<std::size_t>(
      std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
                    [&](const lang::Diagnostic &diagnostic) {
                      return diagnostic.code == code;
                    }));
}

const std::string kPlayer = R"(import std.meta;
struct Player {
  uint32_t health = 100;
  float speed = 1.5;
public:
  Player() = default;
};
)";

void checkExpansion(const std::filesystem::path &root,
                    const std::filesystem::path &directory) {
  // Concrete expansion with a constexpr element binding and a reflected read.
  const auto concrete =
      analyzeSource(root, directory, "concrete.gti", kPlayer + R"(int main() {
  Player player = {.health = uint32_t(7), .speed = 2.0};
  mut uint64_t sum = 0;
  constexpr for (auto field : std::meta::fields_of<Player>()) {
    if constexpr (field.index() == uint64_t(0)) {
      sum = sum + uint64_t(field.get(player));
    }
    static_assert(field.index() < uint64_t(2), "two fields");
  }
  return int(sum);
}
)");
  expect(concrete.diagnostics.empty() && concrete.hirValid,
         "concrete expansion is accepted");

  // A dependent sequence defers in the primary and expands per instance.
  const auto deferred = analyzeSource(root, directory, "deferred.gti",
                                      kPlayer + R"(double total<T>(T& value) {
  mut double sum = 0.0;
  constexpr for (auto field : std::meta::fields_of<T>()) {
    if constexpr (field.type().is_integral()) {
      sum = sum + double(field.get(value));
    } else if constexpr (field.type().is_floating_point()) {
      sum = sum + double(field.get(value));
    }
  }
  return sum;
}
int main() {
  Player player = {.health = uint32_t(7), .speed = 2.0};
  return int(total(player));
}
)");
  expect(deferred.diagnostics.empty() && deferred.hirValid,
         "deferred expansion in a generic function is accepted");

  // Every copy keeps its own locals; a reflection-free sequence works too.
  const auto locals = analyzeSource(root, directory, "locals.gti",
                                    R"(import std.meta;
int main() {
  mut uint64_t total = 0;
  constexpr for (auto element : std::meta::list<int32_t, bool>()) {
    auto local = uint64_t(1);
    total = total + local;
  }
  constexpr for (auto nothing : std::meta::list<>()) {
    total = total + uint64_t(100);
  }
  return int(total);
}
)");
  expect(locals.diagnostics.empty() && locals.hirValid,
         "general sequences, per-copy locals and empty sequences are accepted");

  // Reflected access keeps the field's own mutability and access rules.
  const auto mutation = analyzeSource(root, directory, "mutation.gti",
                                      R"(import std.meta;
struct Counter { mut { uint32_t hits = 0; }  public: Counter() = default; };
int main() {
  mut Counter counter = {.hits = uint32_t(1)};
  Counter frozen = {.hits = uint32_t(1)};
  constexpr for (auto field : std::meta::fields_of<Counter>()) {
    field.get_mut(counter) = field.get(counter) + uint32_t(10);
    field.get_mut(frozen) = uint32_t(0);
  }
  return int(counter.hits);
}
)");
  expect(hasCode(mutation, "GTI-S2112"),
         "get_mut through a read-only owner is rejected");
  expect(!hasCode(mutation, "GTI-S2007"), "writable field access is accepted");

  const auto privateAccess = analyzeSource(root, directory, "private.gti",
                                           R"(import std.meta;
class Secret {
  uint32_t hidden = 1;
public:
  uint32_t shown = 2;
  constexpr uint64_t total() {
    mut uint64_t sum = 0;
    constexpr for (auto field : std::meta::fields_of<Secret>()) {
      sum = sum + uint64_t(field.get(this));
    }
    return sum;
  }
public:
  Secret() = default;
};
int main() {
  Secret secret{};
  mut uint32_t sum = 0;
  constexpr for (auto field : std::meta::fields_of<Secret>()) {
    sum = sum + field.get(secret);
  }
  return int(sum);
}
)");
  expect(
      hasCode(privateAccess, "GTI-S2007"),
      "a private field is inaccessible through reflection outside its class");
  expect(countCode(privateAccess, "GTI-S2007") == 1,
         "the private-field diagnostic is reported once per use site");
}

void checkDiagnostics(const std::filesystem::path &root,
                      const std::filesystem::path &directory) {
  const auto sequence = analyzeSource(root, directory, "sequence.gti",
                                      R"(int main() {
  constexpr for (auto x : 42) { }
  return 0;
}
)");
  expect(hasCode(sequence, "GTI-S2107"), "a non-sequence operand is rejected");

  const auto index =
      analyzeSource(root, directory, "index.gti", kPlayer + R"(int main() {
  Player player{};
  std::meta::field<Player, 5> missing{};
  return int(missing.get(player));
}
)");
  expect(hasCode(index, "GTI-S2111"), "a field index out of range is rejected");

  // Diagnostics inside a copy name the element and repeat once per element
  // only when they differ.
  const auto related =
      analyzeSource(root, directory, "related.gti", kPlayer + R"(int main() {
  constexpr for (auto field : std::meta::fields_of<Player>()) {
    undefined_name = 1;
  }
  return 0;
}
)");
  expect(countCode(related, "GTI-S2001") == 1,
         "an identical diagnostic from every copy is reported once");
  const auto named =
      std::find_if(related.diagnostics.begin(), related.diagnostics.end(),
                   [](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == "GTI-S2001";
                   });
  expect(named != related.diagnostics.end() && !named->related.empty() &&
             named->related.front().message.find("element 0") !=
                 std::string::npos,
         "a diagnostic from a copy names its element");

  std::string many = "import std.meta;\nint main() {\n  constexpr for (auto "
                     "x : std::meta::list<";
  for (int count = 0; count < 1025; ++count) {
    many += count == 0 ? "int32_t" : ", int32_t";
  }
  many += ">()) { }\n  return 0;\n}\n";
  const auto budget = analyzeSource(root, directory, "budget.gti", many);
  expect(hasCode(budget, "GTI-S2108"), "the expansion budget is enforced");

  const auto binding =
      analyzeSource(root, directory, "binding.gti",
                    R"(@compiler_type("meta_list") struct mine<Elements...> {};
int main() { return 0; }
)");
  expect(hasCode(binding, "GTI-S2049"),
         "@compiler_type is rejected outside the trusted unit");
}

void checkFormatter() {
  const std::string source = R"(import std.meta;

int main() {
  constexpr for (auto field : std::meta::fields_of<int>()) {
    return 1;
  }
  return 0;
}
)";
  const lang::Formatter formatter;
  const std::string formatted = formatter.format(source);
  expect(formatted == source, "constexpr for formats as written");
  expect(formatter.format(formatted) == formatted, "formatting is idempotent");
}

const std::string kAttributes = R"(import std.meta;
namespace tags {
[[attribute]] struct key { std::string_view name = "";  public: key() = default; };
[[attribute]] struct skip { public: skip() = default; };
[[attribute]] struct width { uint64_t bytes; };
struct plain { int x = 0;  public: plain() = default; };
}
)";

void checkAttributes(const std::filesystem::path &root,
                     const std::filesystem::path &directory) {
  // Valid applications: positional, designated, zero-argument, several per
  // group, and both compile-time and run-time reads.
  const auto valid =
      analyzeSource(root, directory, "attributes.gti", kAttributes + R"(
[[tags::key("player")]]
struct Player {
  [[tags::key("display"), tags::skip]]
  std::string_view name = "";
  [[tags::width(.bytes = 2)]]
  uint32_t health = 100;
public:
  Player() = default;
};
int main() {
  static_assert(std::meta::type_of<Player>().has_attribute<tags::key>());
  static_assert(std::meta::type_of<Player>().attribute<tags::key>().name == "player");
  mut uint64_t total = 0;
  constexpr for (auto field : std::meta::fields_of<Player>()) {
    total = total + field.attribute_count();
    if constexpr (field.has_attribute<tags::width>()) {
      total = total + field.attribute<tags::width>().bytes;
    }
    if constexpr (field.has_attribute<tags::skip>()) {
      std::string_view key = field.attribute<tags::key>().name;
      if (key != "display") { return 1; }
    }
  }
  return total == 5 ? 0 : 2;
}
)");
  expect(valid.diagnostics.empty(), "attribute applications are accepted");

  const auto invalid =
      analyzeSource(root, directory, "attributes_invalid.gti", kAttributes + R"(
struct A {
  [[tags::plain]] int a = 1;
  [[tags::key("x"), tags::key("y")]] int b = 2;
  [[tags::key("x", "y")]] int c = 3;
  [[tags::width]] int d = 4;
  [[tags::key(.nope = "k")]] int e = 5;
  [[tags::key(1)]] int f = 6;
public:
  A() = default;
};
int main() {
  constexpr for (auto field : std::meta::fields_of<A>()) {
    if constexpr (field.index() == uint64_t(0)) {
      std::string_view k = field.attribute<tags::key>().name;
    }
  }
  return 0;
}
)");
  expect(hasCode(invalid, "GTI-S2113"), "a non-attribute struct is rejected");
  expect(countCode(invalid, "GTI-S2115") == 1,
         "a duplicate application is rejected once");
  expect(countCode(invalid, "GTI-S2114") == 4,
         "too many, unknown, missing and mistyped arguments are rejected");
  expect(hasCode(invalid, "GTI-S2116"),
         "reading an attribute the field does not apply is rejected");
  const auto payload = analyzeSource(root, directory, "attributes_payload.gti",
                                     R"(import std.meta;
enum class data { empty, boxed(int32_t item) };
int main() {
  constexpr for (auto alt : std::meta::alternatives_of<data>()) {
    data d = alt.value();
  }
  return 0;
}
)");
  expect(hasCode(payload, "GTI-S2117"),
         "a payload enumerator has no integral value");
  if (!hasCode(payload, "GTI-S2117")) {
    for (const lang::Diagnostic &diagnostic : payload.diagnostics) {
      std::cerr << "  " << diagnostic.code << ": " << diagnostic.message
                << '\n';
    }
  }

  // Attribute types are structs with public value fields and no constructors.
  const auto shape =
      analyzeSource(root, directory, "attributes_shape.gti", kAttributes + R"(
[[attribute]] class owner { public: int x = 0;  public: owner() = default; };
namespace tags {
[[attribute]] struct withctor { int x; withctor(int v) : x(v) {} };
}
struct B {
  [[tags::withctor(1)]] int b = 2;
public:
  B() = default;
};
int main() { return 0; }
)");
  expect(countCode(shape, "GTI-S2113") == 2,
         "[[attribute]] on a class and an attribute type with a constructor "
         "are rejected");

  // Private fields: inaccessible through reflection unless the owner opts in.
  const std::string accounts = R"(import std.meta;
class Closed {
public:
  int shown = 1;
private:
  int hidden = 2;
public:
  Closed() = default;
};
[[reflect_private]] class Open {
public:
  int shown = 1;
private:
  int hidden = 2;
public:
  Open() = default;
};
int total<std::meta::reflectable T>(T& value) {
  mut int sum = 0;
  constexpr for (auto field : std::meta::fields_of<T>()) {
    sum = sum + field.get(value);
  }
  return sum;
}
)";
  const auto open = analyzeSource(root, directory, "reflect_private.gti",
                                  accounts + R"(int main() {
  Open open{};
  return total(open) - 3;
}
)");
  expect(open.diagnostics.empty(),
         "[[reflect_private]] admits private fields to reflected access");
  const auto closed = analyzeSource(root, directory, "reflect_closed.gti",
                                    accounts + R"(int main() {
  Closed closed{};
  return total(closed) - 3;
}
)");
  expect(hasCode(closed, "GTI-S2029"),
         "a class with private fields does not satisfy std::meta::reflectable");
  const auto direct =
      analyzeSource(root, directory, "reflect_direct.gti", accounts + R"(
int sum_all<T>(T& value) {
  mut int sum = 0;
  constexpr for (auto field : std::meta::fields_of<T>()) {
    sum = sum + field.get(value);
  }
  return sum;
}
int main() {
  Closed closed{};
  return sum_all(closed) - 3;
}
)");
  expect(hasCode(direct, "GTI-S2007"),
         "reflected access to a private field follows direct access rules");
}

void checkAttributeFormatter() {
  // Declared attributes share the class-attribute placement: one group per
  // line before the declaration, kept with it, in the canonical form.
  const std::string source = R"(namespace tags {
  [[attribute]] struct key {
    std::string_view name = "";
  };
}

[[tags::key("player")]] struct Player {
  [[tags::key("display"), tags::skip]] std::string_view name = "";
  [[tags::key(.name = "hp")]] uint32_t health = 100;
};
)";
  const lang::Formatter formatter;
  const std::string formatted = formatter.format(source);
  expect(formatted == source, "declared attributes format as written");
  if (formatted != source) {
    std::cerr << "formatted:\n" << formatted;
  }
  const std::string split = R"([[tags::key("player")]]
struct Player {
  [[tags::key("display"), tags::skip]]
  std::string_view name = "";
public:
  Player() = default;
};
)";
  expect(formatter.format(split) == R"([[tags::key("player")]] struct Player {
  [[tags::key("display"), tags::skip]] std::string_view name = "";
public:
  Player() = default;
};
)",
         "a split attribute line is joined with its declaration");
}

void checkTypeFacts(const std::filesystem::path &root,
                    const std::filesystem::path &directory) {
  for (const auto trustedBodies : {lang::TrustedBodyAnalysis::Complete,
                                   lang::TrustedBodyAnalysis::Reachable}) {
    const auto result =
        analyze(root, root / "tests/fixtures/reflection_type_facts.gti",
                lang::FrontendPhase::Hir, trustedBodies);
    expect(result.diagnostics.empty() && result.hirValid,
           "general reflection lifecycle, classification, safety and exact "
           "type-relation facts match GTI semantics");
    for (const auto &diagnostic : result.diagnostics) {
      std::cerr << "  type facts: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
    }
  }

  const std::string malformed = R"(import std.meta;
int main() {
  auto invalid = gti_internal::meta_type_relation<int32_t, int32_t, 99>();
  return 0;
}
)";
  const auto result =
      analyzeSource(root, directory, "invalid-type-relation.gti", malformed);
  const auto error =
      std::find_if(result.diagnostics.begin(), result.diagnostics.end(),
                   [](const lang::Diagnostic &diagnostic) {
                     return diagnostic.code == "GTI-S2058" &&
                            diagnostic.message.find("Compiler-private name") !=
                                std::string::npos;
                   });
  expect(error != result.diagnostics.end(),
         "application source cannot invoke private reflection relations");
  if (error != result.diagnostics.end()) {
    expect(std::filesystem::path(error->primary.source).filename() ==
                   "invalid-type-relation.gti" &&
               error->primary.start == malformed.find("gti_internal") &&
               error->primary.end ==
                   error->primary.start +
                       std::string_view("gti_internal").size() &&
               !error->related.empty() && !error->hints.empty(),
           "a private reflection relation retains the access diagnostic's "
           "source span, declaration and public-facility hint");
  }

  const auto forwarded = analyzeSource(
      root, directory, "predicate-fact-forwarding.gti", R"(import std.meta;
namespace queries {
  namespace reflected = std::meta;
  using Boolean = bool;
  constexpr bool can_copy<T>() {
    return reflected::is_copy_constructible<T>();
  }
  constexpr bool copied_again<U>() { return can_copy<U>(); }
  constexpr Boolean aliased_result<T>() { return can_copy<T>(); }
  constexpr bool can_transfer<T>() { return reflected::is_transferable<T>(); }
  constexpr bool representation<E, U>() {
    return reflected::is_enum_underlying<E, U>();
  }
}
concept snapshot<T> = queries::copied_again<T>();
concept snapshot_alias<Boolean> = queries::aliased_result<Boolean>();
concept portable<T> = queries::can_transfer<T>();
concept represented_by<E, U> = queries::representation<E, U>();
T copy_value<T>(T& value) requires snapshot<T> { return value; }
T alias_copy<T>(T& value) requires snapshot_alias<T> { return value; }
void needs_transfer<std::transferable T>(T& value) {}
void relay<T>(T& value) requires portable<T> { needs_transfer<T>(value); }
U underlying<E, U>(E value) requires represented_by<E, U> {
  return std::to_underlying<U, E>(value);
}
enum class Code : uint16_t { none = 0, failed = 1 };
int main() {
  int32_t value = 7;
  relay(value);
  return copy_value(value) - alias_copy(value) +
         int(underlying<Code, uint16_t>(Code::none));
}
)");
  expect(forwarded.diagnostics.empty() && forwarded.hirValid,
         "transparent source predicates preserve exact trusted facts through "
         "namespace aliases, renamed type parameters and ordered relations");
  for (const auto &diagnostic : forwarded.diagnostics) {
    std::cerr << "  forwarded predicate: " << diagnostic.code << ": "
              << diagnostic.message << '\n';
  }

  const auto opaque =
      analyzeSource(root, directory, "opaque-predicate-does-not-prove.gti", R"(
namespace lookalike {
  constexpr bool is_transferable<T>() { return true; }
}
concept claimed<T> = lookalike::is_transferable<T>();
void needs_transfer<std::transferable T>(T& value) {}
void relay<T>(T& value) requires claimed<T> { needs_transfer<T>(value); }
struct RawAddress { int32_t* address = nullptr;  public: RawAddress() = default; };
int main() { RawAddress value{}; relay(value); return 0; }
)");
  expect(hasCode(opaque, "GTI-S2029"),
         "an arbitrary true predicate with a familiar public name cannot "
         "invent a transfer safety fact");

  const auto wrongSubject =
      analyzeSource(root, directory, "predicate-wrong-subject.gti", R"(
constexpr bool copyable_int<T>() {
  return std::meta::is_copy_constructible<int32_t>();
}
concept claimed<T> = copyable_int<T>();
void needs_copy<std::copy_constructible T>() {}
void relay<T>() requires claimed<T> { needs_copy<T>(); }
class MoveOnly {
public:
  MoveOnly() {}
  MoveOnly(MoveOnly& other) = delete;
  MoveOnly(MoveOnly&& other) = default;
};
int main() { relay<MoveOnly>(); return 0; }
)");
  expect(hasCode(wrongSubject, "GTI-S2029"),
         "proof of a different type cannot be rebound to a wrapper's generic "
         "parameter by matching its position or spelling");

  const auto shadowedAlias =
      analyzeSource(root, directory, "predicate-alias-shadowing.gti", R"(
namespace declaration_scope {
  using Subject = int32_t;
  constexpr bool can_copy<T>() {
    return std::meta::is_copy_constructible<Subject>();
  }
}
concept claimed<Subject> = declaration_scope::can_copy<Subject>();
void needs_copy<std::copy_constructible T>() {}
void relay<Subject>() requires claimed<Subject> { needs_copy<Subject>(); }
class MoveOnly {
public:
  MoveOnly() {}
  MoveOnly(MoveOnly& other) = delete;
  MoveOnly(MoveOnly&& other) = default;
};
int main() { relay<MoveOnly>(); return 0; }
)");
  expect(hasCode(shadowedAlias, "GTI-S2029"),
         "a caller generic parameter cannot shadow an unrelated type alias "
         "inside the predicate declaration's lexical scope");

  const auto outerShape =
      analyzeSource(root, directory, "reflection-known-outer-shape.gti", R"(
import std.meta;
struct Envelope<T> { T value; };
class Opaque<T> { public: Opaque() = default; };
constexpr int32_t classify<T>() {
  static_assert(std::meta::is_struct<Envelope<T>>());
  static_assert(std::meta::is_class<Opaque<T>>());
  static_assert(!std::meta::is_class<T*>());
  if constexpr (std::meta::is_class<T>()) {
    return 1;
  } else {
    return 2;
  }
}
constexpr int32_t projected<T>() {
  if constexpr (std::meta::is_struct<std::meta::field_type<Envelope<T>, 0>>()) {
    return 1;
  } else {
    return 2;
  }
}
static_assert(classify<Opaque<int32_t>>() == 1);
static_assert(classify<int32_t>() == 2);
static_assert(projected<int32_t>() == 2);
static_assert(projected<Envelope<int32_t>>() == 1);
int main() { return 0; }
)");
  expect(outerShape.diagnostics.empty() && outerShape.hirValid,
         "known outer nominal/pointer categories fold with nested open "
         "arguments while a bare T remains dependent until substitution");
  for (const auto &diagnostic : outerShape.diagnostics) {
    std::cerr << "  outer shape: " << diagnostic.code << ": "
              << diagnostic.message << '\n';
  }
}

void checkPhase3(const std::filesystem::path &root,
                 const std::filesystem::path &directory) {
  // GitHub #5: the owner argument must be the described type (or derive
  // from it); a same-named field of another type is rejected.
  const auto owner =
      analyzeSource(root, directory, "wrong_owner.gti", R"(import std.meta;
struct First { int32_t payload = int32_t(1);  public: First() = default; };
struct Second { double payload = 2.0d;  public: Second() = default; };
class Base { public: int32_t payload = int32_t(3);  public: Base() = default; };
class Derived : public Base { public: int32_t other = int32_t(4);  public: Derived() = default; };
int main() {
  std::meta::field<First, 0> field{};
  Second other{};
  auto wrong = field.get(other);
  std::meta::field<Base, 0> base{};
  Derived derived{};
  auto fine = base.get(derived);
  return 0;
}
)");
  expect(countCode(owner, "GTI-S2003") == 1,
         "an unrelated owner is rejected and a derived owner accepted");

  // GitHub #4: a receiver with effects is kept (semantics accepts it; the
  // runtime fixture proves the evaluation order).
  const auto receiver =
      analyzeSource(root, directory, "receiver.gti", R"(import std.meta;
mut int32_t observations = int32_t(0);
struct Owner { mut { int32_t payload = int32_t(7); }  public: Owner() = default; };
std::meta::field<Owner, 0> make_field() {
  observations = observations + int32_t(1);
  return std::meta::field<Owner, 0>();
}
int main() {
  mut Owner owner{};
  auto value = make_field().get(owner);
  make_field().get_mut(owner) = int32_t(9);
  return value + owner.payload;
}
)");
  expect(receiver.diagnostics.empty(),
         "a side-effecting metaobject receiver is accepted");
  for (const lang::Diagnostic &diagnostic : receiver.diagnostics) {
    std::cerr << "  receiver: " << diagnostic.code << ": " << diagnostic.message
              << '\n';
  }

  // Structural construction is limited to designated-eligible structs.
  const auto construct =
      analyzeSource(root, directory, "construct.gti", R"(import std.meta;
class Guarded {
public:
  Guarded(int32_t start) : value(start) {}
private:
  int32_t value;
};
struct Plain { int32_t a = 0; bool b = false;  public: Plain() = default; };
enum class fail { nope };
struct producer {
  expected<std::meta::field_type<T, I>, fail> produce<T, uint64_t I>(std::meta::field<T, I> field) mut {
    return std::meta::field_type<T, I>(1);
  }
public:
  producer() = default;
};
int main() {
  mut producer source{};
  mut auto fine = std::meta::construct<Plain, fail, producer>(source);
  mut auto rejected = std::meta::construct<Guarded, fail, producer>(source);
  return fine ? 0 : 1;
}
)");
  expect(hasCode(construct, "GTI-S2118"),
         "construct rejects a class with declared constructors");
  if (!hasCode(construct, "GTI-S2118")) {
    for (const lang::Diagnostic &diagnostic : construct.diagnostics) {
      std::cerr << "  construct: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
    }
  }

  // Callable metadata: out-of-range indices and the trait selector.
  const auto callables =
      analyzeSource(root, directory, "callables.gti", R"(import std.meta;
class Counter {
public:
  Counter(uint32_t start) : value(start) {}
  uint32_t get() { return this.value; }
private:
  mut { uint32_t value; }
};
int main() {
  std::meta::method<Counter, 5> missing{};
  std::meta::constructor<Counter, 3> absent{};
  return int(missing.parameter_count() + absent.parameter_count());
}
)");
  expect(countCode(callables, "GTI-S2111") >= 2,
         "method and constructor indices out of range are rejected");

  // Budget hardening: nested expansions over methods and parameters still
  // stop at the per-declaration copy budget with a located diagnostic.
  std::string wide = "import std.meta;\nclass Wide {\npublic:\n";
  for (int index = 0; index < 70; ++index) {
    wide += "  void m" + std::to_string(index) + "(";
    for (int parameter = 0; parameter < 70; ++parameter) {
      wide += (parameter == 0 ? "" : ", ") + std::string("int p") +
              std::to_string(parameter);
    }
    wide += ") {}\n";
  }
  wide += "};\nint main() {\n  mut uint64_t total = 0;\n"
          "  constexpr for (auto m : std::meta::methods_of<Wide>()) {\n"
          "    constexpr for (auto p : m.parameters()) {\n"
          "      total = total + p.index();\n    }\n  }\n  return 0;\n}\n";
  const auto budget = analyzeSource(root, directory, "wide.gti", wide);
  expect(hasCode(budget, "GTI-S2108"),
         "nested callable expansions respect the copy budget");
}

} // namespace

void checkDefaults(const std::filesystem::path &root,
                   const std::filesystem::path &directory) {
  // `default_value()` needs a declared initializer; `has_default()` tells.
  const auto defaults =
      analyzeSource(root, directory, "defaults.gti", R"(import std.meta;
struct Pair { int32_t first = int32_t(1); int32_t second; };
int main() {
  static_assert(std::meta::field<Pair, 0>().has_default());
  static_assert(!std::meta::field<Pair, 1>().has_default());
  int32_t fine = std::meta::field<Pair, 0>().default_value();
  int32_t missing = std::meta::field<Pair, 1>().default_value();
  return fine + missing;
}
)");
  expect(countCode(defaults, "GTI-S2119") == 1,
         "default_value on a field without an initializer is rejected once");
}

void checkClassAssertions(const std::filesystem::path &root,
                          const std::filesystem::path &directory) {
  // A dependent class assertion is evaluated for every concrete instance,
  // and the reflected constants it requires stage through one worker that
  // instance discovery reuses. The driver's Reachable trusted-body mode also
  // wraps each concrete reanalysis in a late-demand probe, so run both modes.
  const std::string box = R"(import std.meta;
class Box<T> {
  static_assert(!std::meta::type<T>().is_void(), "no void");
  mut T value;
public:
  Box(T value) : value(value) {}
  T get() { return this.value; }
};
)";
  for (const lang::TrustedBodyAnalysis trustedBodies :
       {lang::TrustedBodyAnalysis::Complete,
        lang::TrustedBodyAnalysis::Reachable}) {
    const std::string mode =
        trustedBodies == lang::TrustedBodyAnalysis::Reachable ? "reachable"
                                                              : "complete";
    const auto valid = analyzeSource(
        root, directory, "class-assert-" + mode + ".gti",
        box + "int main() { Box<int32_t> b = Box<int32_t>(1); return 0; }\n",
        trustedBodies);
    expect(valid.diagnostics.empty() && valid.hirValid,
           mode + ": a reflected class assertion holds for an instance whose "
                  "class and constructor both reanalyse it");

    // The failed assertion is the instance's only error: neither its void
    // field nor the method used through it reports a consequence.
    const auto rejected = analyzeSource(
        root, directory, "class-assert-void-" + mode + ".gti",
        box + "int unwrap(Box<void> &box) { box.get(); return 0; }\n"
              "int main() { return 0; }\n",
        trustedBodies);
    const lang::Diagnostic *failure =
        rejected.diagnostics.empty() ? nullptr : &rejected.diagnostics.front();
    expect(!rejected.hirValid && rejected.diagnostics.size() == 1 &&
               failure != nullptr && failure->code == "GTI-S2079" &&
               failure->message == "Static assertion failed. no void" &&
               std::any_of(failure->related.begin(), failure->related.end(),
                           [](const lang::RelatedDiagnostic &related) {
                             return related.message ==
                                    "Concrete substitution: 'T' = 'void'.";
                           }),
           mode + ": an invalid type argument is rejected by the class "
                  "assertion's own message, as the instance's only error");

    // Reflecting on the instance itself, or on an instance whose assertion
    // reflects back, reanalyses it in the staged worker, and so does an
    // assertion in a destructor that reflects on its class. The enclosing
    // reanalysis owns the assertion, which is not evaluated again there.
    const auto reflective =
        analyzeSource(root, directory, "class-assert-self-" + mode + ".gti",
                      R"(import std.meta;
class Node<T> {
  static_assert(std::meta::type<Node<T>>().field_count() == uint64_t(1),
                "one field");
  T value;
public:
  Node(T value) : value(value) {}
};
class Left<T> {
  static_assert(std::meta::type<Right<T>>().field_count() == uint64_t(1),
                "right has one field");
  T value;
public:
  Left(T value) : value(value) {}
};
class Right<T> {
  static_assert(std::meta::type<Left<T>>().field_count() == uint64_t(1),
                "left has one field");
  T value;
public:
  Right(T value) : value(value) {}
};
class Guard<T> {
  T value;
public:
  Guard(T value) : value(value) {}
  ~Guard() {
    static_assert(std::meta::type<Guard<T>>().field_count() == uint64_t(1),
                  "guard has one field");
  }
};
int main() {
  Node<int32_t> node = Node<int32_t>(1);
  Left<int32_t> left = Left<int32_t>(2);
  Right<int32_t> right = Right<int32_t>(3);
  Guard<int32_t> guard = Guard<int32_t>(4);
  return 0;
}
)",
                      trustedBodies);
    expect(reflective.diagnostics.empty() && reflective.hirValid,
           mode + ": an assertion may reflect on its own class instance or "
                  "on one that reflects back");

    const auto miscounted = analyzeSource(
        root, directory, "class-assert-self-false-" + mode + ".gti",
        R"(import std.meta;
class Node<T> {
  static_assert(std::meta::type<Node<T>>().field_count() == uint64_t(2),
                "two fields");
  T value;
public:
  Node(T value) : value(value) {}
};
int main() { Node<int32_t> node = Node<int32_t>(1); return 0; }
)",
        trustedBodies);
    expect(miscounted.diagnostics.size() == 1 &&
               miscounted.diagnostics.front().message ==
                   "Static assertion failed. two fields",
           mode + ": a false self-reflecting assertion reports its message "
                  "once");

    // Constant evaluation and initializer-effect analysis reanalyse class
    // instances in isolated workers; their assertions behave the same there.
    const auto staged =
        analyzeSource(root, directory, "class-assert-staged-" + mode + ".gti",
                      R"(import std.meta;
class Cell<T> {
  static_assert(!std::meta::type<T>().is_void(), "no void cell");
  T value;
public:
  constexpr Cell(T value) : value(value) {}
  constexpr T get() { return value; }
};
constexpr int32_t read_cell() {
  Cell<int32_t> cell = Cell<int32_t>(7);
  return cell.get();
}
static_assert(read_cell() == 7);
Cell<int32_t> global_cell = Cell<int32_t>(3);
int main() { return global_cell.get() - 3; }
)",
                      trustedBodies);
    expect(staged.diagnostics.empty() && staged.hirValid,
           mode + ": class assertions hold for instances reached by constant "
                  "evaluation and by a global initializer");

    const auto stagedVoid = analyzeSource(
        root, directory, "class-assert-staged-void-" + mode + ".gti",
        R"(import std.meta;
class Tag<T> {
  static_assert(!std::meta::type<T>().is_void(), "no void tag");
public:
  constexpr Tag() {}
};
constexpr int32_t tag<T>() {
  Tag<T> value = Tag<T>();
  return 1;
}
static_assert(tag<int32_t>() == 1);
static_assert(tag<void>() == 1);
int main() { return 0; }
)",
        trustedBodies);
    const auto stagedFailure = std::find_if(
        stagedVoid.diagnostics.begin(), stagedVoid.diagnostics.end(),
        [](const lang::Diagnostic &diagnostic) {
          return diagnostic.code == "GTI-S2079";
        });
    expect(countCode(stagedVoid, "GTI-S2079") == 1 &&
               stagedFailure != stagedVoid.diagnostics.end() &&
               stagedFailure->message == "Static assertion failed. no void tag",
           mode + ": an instance formed only by constant evaluation is still "
                  "rejected by its class assertion");
  }
}

int main() {
  const std::filesystem::path root =
      std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("gti-reflection-" + std::to_string(::getpid()));
  std::filesystem::create_directories(directory);
  checkExpansion(root, directory);
  checkDiagnostics(root, directory);
  checkFormatter();
  checkAttributes(root, directory);
  checkAttributeFormatter();
  checkTypeFacts(root, directory);
  checkPhase3(root, directory);
  checkDefaults(root, directory);
  checkClassAssertions(root, directory);
  std::filesystem::remove_all(directory);
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  return 0;
}
