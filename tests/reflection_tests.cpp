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
        lang::FrontendPhase stopAfter = lang::FrontendPhase::Hir) {
  lang::FrontendOptions options;
  options.stopAfter = stopAfter;
  options.analyzeRecoveredProgram = true;
  return lang::Frontend(options).analyze(entry, std::nullopt,
                                         {root / "stdlib/prelude.gti"}, {},
                                         {root / "stdlib"});
}

lang::FrontendResult analyzeSource(const std::filesystem::path &root,
                                   const std::filesystem::path &directory,
                                   const std::string &name,
                                   const std::string &source) {
  const std::filesystem::path path = directory / name;
  std::ofstream(path) << source;
  return analyze(root, path);
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

const std::string kPlayer = R"(#include <std/meta>
struct Player {
  uint32_t health = 100;
  float speed = 1.5;
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
                                    R"(#include <std/meta>
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
                                      R"(#include <std/meta>
struct Counter { mut { uint32_t hits = 0; } };
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
                                           R"(#include <std/meta>
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

  std::string many = "#include <std/meta>\nint main() {\n  constexpr for (auto "
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
  const std::string source = R"(#include <std/meta>

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

const std::string kAttributes = R"(#include <std/meta>
namespace tags {
[[attribute]] struct key { std::string_view name = ""; };
[[attribute]] struct skip {};
[[attribute]] struct width { uint64_t bytes; };
struct plain { int x = 0; };
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
                                     R"(#include <std/meta>
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
[[attribute]] class owner { public: int x = 0; };
namespace tags {
[[attribute]] struct withctor { int x; withctor(int v) : x(v) {} };
}
struct B {
  [[tags::withctor(1)]] int b = 2;
};
int main() { return 0; }
)");
  expect(countCode(shape, "GTI-S2113") == 2,
         "[[attribute]] on a class and an attribute type with a constructor "
         "are rejected");

  // Private fields: inaccessible through reflection unless the owner opts in.
  const std::string accounts = R"(#include <std/meta>
class Closed {
public:
  int shown = 1;
private:
  int hidden = 2;
};
[[reflect_private]] class Open {
public:
  int shown = 1;
private:
  int hidden = 2;
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
};
)";
  expect(formatter.format(split) == R"([[tags::key("player")]] struct Player {
  [[tags::key("display"), tags::skip]] std::string_view name = "";
};
)",
         "a split attribute line is joined with its declaration");
}

void checkPhase3(const std::filesystem::path &root,
                 const std::filesystem::path &directory) {
  // GitHub #5: the owner argument must be the described type (or derive
  // from it); a same-named field of another type is rejected.
  const auto owner =
      analyzeSource(root, directory, "wrong_owner.gti", R"(#include <std/meta>
struct First { int32_t payload = int32_t(1); };
struct Second { double payload = 2.0d; };
class Base { public: int32_t payload = int32_t(3); };
class Derived : public Base { public: int32_t other = int32_t(4); };
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
      analyzeSource(root, directory, "receiver.gti", R"(#include <std/meta>
mut int32_t observations = int32_t(0);
struct Owner { mut { int32_t payload = int32_t(7); } };
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
      analyzeSource(root, directory, "construct.gti", R"(#include <std/meta>
class Guarded {
public:
  Guarded(int32_t start) : value(start) {}
private:
  int32_t value;
};
struct Plain { int32_t a = 0; bool b = false; };
enum class fail { nope };
struct producer {
  expected<std::meta::field_type<T, I>, fail> produce<T, uint64_t I>(std::meta::field<T, I> field) mut {
    return std::meta::field_type<T, I>(1);
  }
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
      analyzeSource(root, directory, "callables.gti", R"(#include <std/meta>
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
  std::string wide = "#include <std/meta>\nclass Wide {\npublic:\n";
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
  checkPhase3(root, directory);
  std::filesystem::remove_all(directory);
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  return 0;
}
