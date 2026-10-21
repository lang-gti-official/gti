#include "cpp_backend_test_support.h"
#include "gti/frontend.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

// Conversion operators: `operator Target()` is called only by an explicit
// `Target(value)` unless declared `implicit`, which also admits declaration
// initialization. Every rule is exercised through unrelated user types.
int main() {
  int failures = 0;
  const auto expect = [&](bool condition, const std::string &message) {
    if (!condition) {
      std::cerr << message << '\n';
      ++failures;
    }
  };
  // Sources that include the standard library resolve it from this checkout.
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto analyze = [&](const std::string &source) {
    return lang::Frontend().analyze(root / "tests" / "conversion-operators.gti",
                                    source, {root / "stdlib" / "prelude.gti"},
                                    {}, {root / "stdlib"});
  };
  const auto accepted = [&](const std::string &source,
                            const std::string &message) {
    const auto result = analyze(source);
    expect(result.canGenerateCode(), message);
    if (!result.canGenerateCode()) {
      for (const auto &d : result.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
      return;
    }
    const std::string cpp = gti_test::emitCpp(result).contents;
    expect(cpp.find("__gti_operator_conversion") != std::string::npos ||
               cpp.find("__gti_operator_bool") != std::string::npos,
           message + " (the selected operator must be emitted as a call)");
  };
  // The located owning diagnostic, its code and a fragment of its wording.
  const auto rejected = [&](const std::string &source, const char *code,
                            const std::string &fragment) {
    const auto result = analyze(source);
    const bool located = std::any_of(
        result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto &d) {
          const bool hinted = std::any_of(
              d.hints.begin(), d.hints.end(), [&](const std::string &hint) {
                return hint.find(fragment) != std::string::npos;
              });
          return d.code == code && !d.primary.source.empty() &&
                 d.primary.end > d.primary.start &&
                 (d.message.find(fragment) != std::string::npos || hinted);
        });
    if (!located)
      for (const auto &d : result.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
    expect(!result.canGenerateCode(),
           "invalid source must not reach codegen: " + fragment);
    expect(located,
           std::string("missing ") + code + " with '" + fragment + "'");
  };

  const std::string ids = R"gti(
struct UserId {
  int64_t value;
  UserId(int64_t raw) : value(raw) {}
};
struct RawId {
  int64_t raw;
  RawId(int64_t value) : raw(value) {}
  operator UserId() { return UserId(this.raw); }
  implicit operator int64_t() { return this.raw; }
};
)gti";

  // Explicit forms: a class target, a keyword target, an alias of each, and
  // bool. A declaration uses only the implicit operator.
  accepted(ids + R"gti(
using Count = int64_t;
using Account = UserId;
struct Flag {
  bool on;
  Flag(bool value) : on(value) {}
  operator bool() { return this.on; }
};
int main() {
  RawId raw{int64_t(7)};
  UserId user = UserId(raw);
  Account account = Account(raw);
  int64_t bits = int64_t(raw);
  Count counted = Count(raw);
  int64_t implicit_bits = raw;
  Flag flag{true};
  bool explicit_flag = bool(flag);
  if (flag) {
    return int(user.value + account.value + bits + counted + implicit_bits);
  }
  return explicit_flag ? 1 : 0;
}
)gti",
           "explicit conversions and an implicit declaration must resolve");

  // A parenthesized fresh value or conditional join is still the value the
  // condition's operator bool converts.
  accepted(R"gti(
struct Flag {
  bool on;
  Flag(bool value) : on(value) {}
  operator bool() { return this.on; }
};
Flag make(bool value) { return Flag(value); }
bool choose(bool yes) {
  mut int total = 0;
  if ((make(true))) { total += 1; }
  if ((yes ? make(true) : make(false))) { total += 2; }
  total += (yes ? make(true) : make(false)) ? 4 : 0;
  total += (make(true)) ? 8 : 0;
  return total == 15;
}
int main() { return choose(true) ? 0 : 1; }
)gti",
           "a parenthesized condition keeps its operator bool conversion");

  // Declaration conversion everywhere a declaration's initializer is
  // analyzed: locals, namespace bindings, field defaults, field constructor
  // initializers and static members, plus `implicit operator bool()`.
  accepted(R"gti(
struct Meters {
  int value;
  Meters(int amount) : value(amount) {}
};
struct Feet {
  int value;
  Feet(int amount) : value(amount) {}
  implicit operator Meters() { return Meters(this.value / 3); }
  implicit operator bool() { return this.value != 0; }
};
Meters global_length = Feet(9);
Feet standard_lap = Feet(30);
class Track {
  Meters lap = standard_lap;
  Meters start;
public:
  Track(Feet first) : start(first) {}
  static Meters spare = Feet(3);
  int total() { return this.lap.value + this.start.value; }
};
int main() {
  Feet feet{12};
  Meters local = feet;
  bool nonzero = feet;
  Track track{Feet(6)};
  int implicit = 1;
  return nonzero ? local.value + global_length.value + track.total() +
                       Track::spare.value - implicit
                 : 0;
}
)gti",
           "declarations must convert through implicit operators");

  // Receivers follow ordinary member calls: a temporary or explicitly moved
  // owner selects the consuming operator, a mutable place the `mut` one.
  accepted(R"gti(
#include <std/string>
class Builder {
  mut std::string text;
public:
  Builder(std::string_view start) : text(start) {}
  implicit operator std::string() && { return std::move(this.text); }
};
struct Counter {
  mut int calls = 0;
  operator int() { return this.calls; }
  operator int() mut {
    this.calls++;
    return this.calls;
  }
};
Builder make() { return Builder("made"); }
int main() {
  std::string made = make();
  Builder named{"named"};
  std::string moved = std::move(named);
  std::string explicit_made = std::string(Builder("explicit"));
  mut Counter counter{};
  Counter fixed{};
  int first = int(counter);
  int read = int(fixed);
  std::println(made);
  std::println(moved);
  std::println(explicit_made);
  return first + read;
}
)gti",
           "consuming and mutable receivers must select their operators");

  // Inheritance and generics: a derived receiver uses its base's operator, a
  // class template converts to its parameter, and a generic body uses an
  // implicit conversion whose source is a class.
  accepted(R"gti(
class Shape {
public:
  virtual int sides() { return 0; }
  operator int() { return this.sides(); }
};
class Square : public Shape {
public:
  int sides() override { return 4; }
};
struct Box<T> {
  T value;
  Box(T initial) : value(initial) {}
  implicit operator T() { return this.value; }
};
struct Wrapper<T> {
  T inner;
  Wrapper(T value) : inner(value) {}
  operator Box<T>() { return Box<T>(this.inner); }
};
T unwrap<T>(Box<T> box) {
  T value = box;
  return value;
}
int main() {
  Square square{};
  Wrapper<int> wrapper{6};
  Box<int> box = Box<int>(wrapper);
  int direct = box;
  return int(square) + direct + unwrap(Box<int>(2));
}
)gti",
           "inherited and generic conversions must resolve");

  // Constant evaluation calls a constexpr operator through the same selected
  // call, and reflection names each operator by its target.
  accepted(R"gti(
#include <std/meta>
#include <std/string>
struct UserId {
  int64_t value;
  UserId(int64_t raw) : value(raw) {}
};
struct Meters {
  int value;
  constexpr Meters(int amount) : value(amount) {}
  constexpr operator int() { return this.value * 2; }
  implicit constexpr operator int64_t() { return int64_t(this.value); }
  operator UserId() { return UserId(int64_t(this.value)); }
  implicit operator std::string() { return std::string("meters"); }
  implicit operator bool() { return true; }
};
constexpr int folded = int(Meters(4));
static_assert(folded == 8);
int main() {
  constexpr for (auto op : std::meta::operators_of<Meters>()) {
    if constexpr (op.result().is_bool()) {
      static_assert(op.name() == "operatorbool");
    } else if constexpr (op.index() == uint64_t(0)) {
      static_assert(op.name() == "operator int32_t");
    } else if constexpr (op.index() == uint64_t(1)) {
      static_assert(op.name() == "operator int64_t");
    } else if constexpr (op.index() == uint64_t(2)) {
      static_assert(op.name() == "operator UserId");
    } else {
      static_assert(op.name() == "operator std::string");
    }
  }
  Meters meters{3};
  int64_t runtime = meters;
  return folded + int(runtime) + int(UserId(meters).value);
}
)gti",
           "constexpr conversions and reflected operator names must resolve");
  rejected(R"gti(
struct Meters {
  int value;
  constexpr Meters(int amount) : value(amount) {}
  implicit constexpr operator int64_t() { return int64_t(this.value); }
};
constexpr int64_t widened = Meters(5);
int main() { return 0; }
)gti",
           "GTI-S2003", "Cannot initialize 'widened'");

  accepted(R"gti(
struct Reading {
  int value;
  Reading(int source) : value(source) {}
  implicit operator int() { return this.value; }
};
int read(Reading value) { return value; }
int main() { return read(Reading(7)) - 7; }
)gti",
           "a declared return uses one implicit conversion operator");
  accepted(R"gti(
struct Source {
  int value = 7;
  implicit operator int() { return this.value; }
};
T convert<T>(Source value) { return value; }
int main() { return convert<int>(Source()) - 7; }
)gti",
           "a concrete generic return retains its implicit operator selection");
  rejected(R"gti(
struct End {
  End(int value) {}
  implicit End(Source& value) {}
};
struct Source { implicit operator End() { return End(7); } };
End make(Source& value) { return value; }
int main() { return 0; }
)gti",
           "GTI-S2122", "ambiguous");
  rejected(R"gti(
#include <std/optional>
#include <std/string>
std::optional<std::string> label() { return "ready"; }
int main() { return 0; }
)gti",
           "GTI-S2003", "Cannot return");
  rejected(R"gti(
#include <std/optional>
#include <std/string>
std::optional<std::string> label(std::string named) { return named; }
int main() { return 0; }
)gti",
           "GTI-S2003", "Cannot return 'named' by copy");
  rejected(R"gti(
#include <std/optional>
int take(std::optional<int> value) { return 0; }
int main() { return take(7); }
)gti",
           "GTI-S2003", "parameter requires");
  // Calls, assignment and operands still require exact types.
  rejected(ids + "int take(UserId id) { return 0; }\n"
                 "int main() { RawId raw{int64_t(1)}; return take(raw); }\n",
           "GTI-S2003", "parameter requires 'UserId'");
  rejected(ids + "UserId give(RawId raw) { return raw; }\n"
                 "int main() { return 0; }\n",
           "GTI-S2003", "Cannot return");
  rejected(ids + "int main() { RawId raw{int64_t(1)}; mut UserId id = "
                 "UserId(raw); id = raw; return 0; }\n",
           "GTI-S2003", "Cannot assign");
  rejected(ids + "int main() { RawId raw{int64_t(1)}; auto same = raw; "
                 "UserId id = same; return 0; }\n",
           "GTI-S2003", "write 'UserId(...)' to convert explicitly");
  rejected(ids + "int main() { RawId raw{int64_t(1)}; int64_t sum = raw + "
                 "int64_t(1); return 0; }\n",
           "GTI-S2022", "does not define");
  // Braces select only constructors and say how to use the operator.
  rejected(ids + "int main() { RawId raw{int64_t(1)}; UserId id{raw}; "
                 "return 0; }\n",
           "GTI-S2012", "brace construction never calls");
  // One step only: no chain through an intermediate type.
  rejected(R"gti(
struct C { int raw = 7; };
struct B { implicit operator C() { return C(); } };
struct A { implicit operator B() { return B(); } };
int main() { A start{}; C chained = start; return chained.raw; }
)gti",
           "GTI-S2003", "Cannot initialize 'chained'");
  // A class source without an operator to the target.
  rejected(ids + "int main() { RawId raw{int64_t(1)}; double d = "
                 "double(raw); return 0; }\n",
           "GTI-S2014", "declares no conversion operator to 'double'");

  // A constructor path and an operator path together are ambiguous; access
  // and receiver requirements never let one path fall back to the other.
  const std::string ambiguous = R"gti(
struct Target {
  int value;
  Target(int raw) : value(raw) {}
  implicit Target(Source& source) : value(1) {}
};
struct Source {
  int raw = 2;
  implicit operator Target() { return Target(this.raw); }
};
)gti";
  rejected(ambiguous + "int main() { Source source{}; Target target = "
                       "Target(source); return target.value; }\n",
           "GTI-S2122", "Conversion of 'Source' to 'Target' is ambiguous");
  rejected(ambiguous + "int main() { Source source{}; Target target = source; "
                       "return target.value; }\n",
           "GTI-S2122",
           "Initialization of 'Target' from 'Source' is ambiguous");
  rejected(R"gti(
class Secret {
  int hidden = 5;
  operator int() { return this.hidden; }
public:
  Secret() {}
};
int main() { Secret secret{}; return int(secret); }
)gti",
           "GTI-S2007", "The conversion of 'Secret' to 'int32_t' is private");
  rejected(R"gti(
#include <std/string>
class Builder {
  mut std::string text;
public:
  Builder(std::string_view start) : text(start) {}
  implicit operator std::string() && { return std::move(this.text); }
};
int main() { Builder named{"named"}; std::string text = named; return 0; }
)gti",
           "GTI-S2022", "A consuming conversion operator requires");
  rejected(R"gti(
struct Counter {
  mut int calls = 0;
  operator int() mut {
    this.calls++;
    return this.calls;
  }
};
int main() { Counter fixed{}; return int(fixed); }
)gti",
           "GTI-S2000", "Conversion to 'int32_t' requires a mutable receiver");

  // Declarations: one owned value target per receiver, no parameters, and no
  // virtual dispatch yet.
  const auto declaration = [&](const std::string &member,
                               const std::string &fragment,
                               const char *code = "GTI-S2022") {
    rejected("#include <std/string>\nusing Count = int;\nusing Truth = "
             "bool;\nstruct Holder {\n  std::string text;\n  int value = "
             "0;\n  Holder() : text(\"held\") {}\n  " +
                 member + "\n};\nint main() { return 0; }\n",
             code, fragment);
  };
  declaration("operator void() {}", "must produce a value, not void");
  declaration("operator Holder() { return Holder(); }",
              "cannot target its own class");
  declaration("operator int&() { return this.value; }",
              "reference conversions are not supported yet");
  declaration("operator std::string_view() { return this.text.view(); }",
              "cannot produce borrowed state yet");
  declaration("operator Truth() { return true; }",
              "Declare the boolean conversion as 'operator bool()'");
  declaration("operator int(int extra) { return extra; }",
              "takes no parameters");
  declaration("operator Count() { return 1; }\n  operator int() { return 2; }",
              "Duplicate conversion operator to 'int32_t'", "GTI-S2011");
  declaration("virtual operator int() { return 1; }", "cannot be virtual yet");
  rejected(R"gti(
interface Describable {
  operator int();
};
int main() { return 0; }
)gti",
           "GTI-S2022", "cannot be virtual yet");
  rejected("struct Ordering {\n  constexpr implicit operator int() { return 3; "
           "}\n};\nint main() { return 0; }\n",
           "GTI-P0001", "Write 'implicit' before 'constexpr'");
  // A generic body is checked before its instances: a type-parameter target
  // stays a numeric conversion.
  rejected(R"gti(
struct Meters { int value = 1; operator int() { return this.value; } };
T convert<T>(Meters meters) { return T(meters); }
int main() { return convert<int>(Meters()); }
)gti",
           "GTI-S2014", "numeric conversions require a numeric value");

  // A conversion's operator is part of an initializer's program effects:
  // declarations, explicit conversions and a condition's operator bool.
  rejected(R"gti(
class Globals {
public:
  struct Source {
    implicit operator int() { return Globals::later; }
  };
  static int32_t early = Source();
  static mut int32_t later = 5;
};
int main() { return Globals::early; }
)gti",
           "GTI-S2068", "Initializer for 'Globals::early'");
  rejected(R"gti(
class Globals {
public:
  struct Source {
    operator int() { return Globals::later; }
  };
  static int32_t early = int(Source());
  static mut int32_t later = 5;
};
int main() { return Globals::early; }
)gti",
           "GTI-S2068", "Initializer for 'Globals::early'");
  rejected(R"gti(
class Globals {
public:
  struct Source {
    operator bool() { return Globals::later > 0; }
  };
  static bool tested = Source() ? true : false;
  static mut int32_t later = 5;
};
int main() { return Globals::tested ? 1 : 0; }
)gti",
           "GTI-S2068", "Initializer for 'Globals::tested'");

  if (failures != 0) {
    std::cerr << failures << " conversion operator check(s) failed\n";
    return 1;
  }
  std::cout << "conversion operator selection, contexts, receivers, "
               "declarations and effects passed\n";
  return 0;
}
