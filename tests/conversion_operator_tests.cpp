#include "cpp_backend_test_support.h"
#include "gti/frontend.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

// Conversion operators: `operator Target()` is called only by an explicit
// `Target(value)` unless declared `implicit`, which also admits declaration
// initialization and returns. Every rule is exercised through unrelated user
// types.
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

  // A converted return lowers as the selected operator's receiver-only call
  // on the returned value, as a declaration's does: one Move of an eligible
  // operand when the operator consumes it, the place itself otherwise. Each
  // instance of the named function reports whether its operand moved.
  const auto returnMoves = [&](const lang::FrontendResult &result,
                               const std::string &name) {
    std::vector<bool> moves;
    for (const lang::StmtPtr &declaration : result.program.declarations()) {
      const auto *function =
          dynamic_cast<const lang::FunctionDecl *>(declaration.get());
      if (function == nullptr || function->name().lexeme != name ||
          function->body() == nullptr)
        continue;
      const lang::ReturnStmt *returned = nullptr;
      for (const lang::StmtPtr &statement : function->body()->statements())
        if (const auto *candidate =
                dynamic_cast<const lang::ReturnStmt *>(statement.get()))
          returned = candidate;
      const lang::FunctionInfo *info = result.semantics.findFunction(*function);
      if (returned == nullptr || !returned->value() || info == nullptr)
        continue;
      for (const lang::HirFunctionInstance &instance :
           result.hir.functionInstances()) {
        if (instance.declaration != info->id)
          continue;
        for (const lang::HirStatement &statement : instance.body.statements) {
          if (statement.kind != lang::HirStatementKind::Return ||
              statement.source != returned || !statement.value)
            continue;
          const lang::HirValue *call =
              instance.body.findValue(*statement.value);
          const lang::HirValue *operand =
              call != nullptr && call->kind == lang::HirValueKind::Call &&
                      call->receiver && call->functionTarget
                  ? instance.body.findValue(*call->receiver)
                  : nullptr;
          if (operand != nullptr && operand->source == returned->value().get())
            moves.push_back(operand->kind == lang::HirValueKind::Move);
        }
      }
    }
    return moves;
  };
  const auto acceptedReturns = [&](const std::string &source,
                                   const std::string &message) {
    auto result = analyze(source);
    expect(result.canGenerateCode(), message);
    if (!result.canGenerateCode())
      for (const auto &d : result.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
    return result;
  };

  // A return converts its value exactly as `R result = value;` would, through
  // an implicit operator to an owned value type, including `bool` and numeric
  // targets. The operator's receiver follows member-call rules: a temporary
  // is borrowed for the call, a named place borrowed or mutated in place, and
  // a consuming operator takes an owned temporary or a whole move-only local
  // or by-value parameter that the return moves implicitly (ADR 023).
  {
    auto result = acceptedReturns(R"gti(
#include <std/string>
struct UserId {
  int64_t value;
  UserId(int64_t raw) : value(raw) {}
};
struct RawId {
  int64_t raw;
  RawId(int64_t value) : raw(value) {}
  implicit operator UserId() { return UserId(this.raw); }
  implicit operator bool() { return this.raw != int64_t(0); }
};
class Builder {
  mut std::string text;
public:
  Builder(std::string_view start) : text(start) {}
  Builder(Builder& other) = delete;
  Builder(Builder&& other) = default;
  implicit operator std::string() && { return std::move(this.text); }
};
struct Counter {
  mut int calls = 0;
  implicit operator int() mut {
    this.calls++;
    return this.calls;
  }
};
UserId temporary() { return RawId(int64_t(4)); }
UserId borrowed(RawId raw) { return raw; }
bool truth(RawId raw) { return raw; }
std::string built() {
  Builder builder{"local"};
  return builder;
}
std::string parameter(Builder builder) { return (builder); }
std::string fresh() { return Builder("fresh"); }
int counted() {
  mut Counter counter{};
  return counter;
}
int main() {
  std::string local = built();
  std::string moved = parameter(Builder("param"));
  std::string made = fresh();
  return int(temporary().value + borrowed(RawId(int64_t(2))).value) +
         (truth(RawId(int64_t(1))) ? 1 : 0) + counted() +
         int(local.size() + moved.size() + made.size());
}
)gti",
                                  "returns must convert through implicit "
                                  "operators");
    expect(returnMoves(result, "temporary") == std::vector<bool>{false} &&
               returnMoves(result, "borrowed") == std::vector<bool>{false} &&
               returnMoves(result, "truth") == std::vector<bool>{false} &&
               returnMoves(result, "built") == std::vector<bool>{true} &&
               returnMoves(result, "parameter") == std::vector<bool>{true} &&
               returnMoves(result, "fresh") == std::vector<bool>{false} &&
               returnMoves(result, "counted") == std::vector<bool>{false},
           "a consuming operator must receive one implicit Move of an "
           "eligible operand, and every other return its own value");
  }

  // A lambda with a declared return type converts like a function, and a
  // generic body's return converts in each concrete instance.
  {
    auto result = acceptedReturns(R"gti(
struct Meters {
  int value;
  Meters(int amount) : value(amount) {}
};
struct Feet {
  int value;
  Feet(int amount) : value(amount) {}
  implicit operator Meters() { return Meters(this.value / 3); }
};
struct Box<T> {
  mut T value;
  Box(T initial) : value(std::move(initial)) {}
  implicit operator T() && { return std::move(this.value); }
};
class Token {
public:
  int id;
  Token(int raw) : id(raw) {}
  Token(Token& other) = delete;
  Token(Token&& other) = default;
};
Token unbox(Box<Token> box) { return box; }
T unwrap<T>(Box<T> box) { return std::move(box); }
int main() {
  auto measure = [](int feet) -> Meters { return Feet(feet); };
  auto named = [](int feet) -> Meters {
    Feet local{feet};
    return local;
  };
  Meters first = measure(9);
  Meters second = named(12);
  Token token = unbox(Box<Token>(Token(5)));
  return first.value + second.value + token.id +
         unwrap(Box<int>(1)) + unwrap(Box<Token>(Token(2))).id;
}
)gti",
                                  "lambdas and generic instances must convert "
                                  "returns");
    expect(returnMoves(result, "unbox") == std::vector<bool>{true},
           "a move-only by-value parameter must move into a consuming "
           "operator");
  }

  // Calls, assignment, `auto` inference, operands and braces never convert,
  // and a conversion operator without `implicit` is explicit at a return too.
  rejected(ids + "int take(UserId id) { return 0; }\n"
                 "int main() { RawId raw{int64_t(1)}; return take(raw); }\n",
           "GTI-S2003", "parameter requires 'UserId'");
  rejected(ids + "UserId give(RawId raw) { return raw; }\n"
                 "int main() { return 0; }\n",
           "GTI-S2003", "Cannot return");
  rejected(ids + "UserId give(RawId raw) { return raw; }\n"
                 "int main() { return 0; }\n",
           "GTI-S2003", "converts a returned value; write 'UserId(...)'");
  // A consuming operator never consumes a named owner the return does not
  // move: a copyable local, a reference binding or a field. Movement needs a
  // whole move-only local or by-value parameter.
  const std::string builder = R"gti(
#include <std/string>
struct Coins {
  mut int count = 3;
  implicit operator int() && { return this.count; }
};
class Pinned {
  mut std::string text;
public:
  Pinned(std::string_view start) : text(start) {}
  Pinned(Pinned& other) = delete;
  Pinned(Pinned&& other) = default;
  implicit operator std::string() && { return std::move(this.text); }
};
)gti";
  rejected(builder + "int copyable() { Coins coins{}; return coins; }\n"
                     "int main() { return 0; }\n",
           "GTI-S2022", "A consuming conversion operator requires");
  rejected(builder + "std::string aliased(Pinned& pinned) { return pinned; }\n"
                     "int main() { return 0; }\n",
           "GTI-S2022", "A consuming conversion operator requires");
  rejected(builder + "class Shop {\n  Pinned pinned;\npublic:\n  Shop() : "
                     "pinned(Pinned(\"b\")) {}\n  std::string take() { return "
                     "this.pinned; }\n};\nint main() { return 0; }\n",
           "GTI-S2022", "A consuming conversion operator requires");
  // Reference, text-view, void and deduced returns keep their own rules.
  rejected(ids + "int64_t& bits(RawId& raw) { return raw; }\n"
                 "int main() { return 0; }\n",
           "GTI-S2017", "Reference return requires an exact 'int64_t' value");
  rejected(builder + "std::string_view view(Pinned pinned) { return pinned; "
                     "}\nint main() { return 0; }\n",
           "GTI-S2003",
           "Cannot return a value of type 'Pinned' from a function returning "
           "'std::string_view'");
  rejected(ids + "void none(RawId raw) { return raw; }\n"
                 "int main() { return 0; }\n",
           "GTI-S2000", "Void function cannot return a value");
  rejected(ids + "auto deduced() { return RawId(int64_t(1)); }\n"
                 "int main() { return 0; }\n",
           "GTI-S2028",
           "'auto' is only valid for an initialized local binding");
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
  rejected(ambiguous + "Target give(Source source) { return source; }\n"
                       "int main() { return give(Source()).value; }\n",
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
               "declarations, returns and effects passed\n";
  return 0;
}
