"""Matrix: user-defined conversions, explicit default construction and protocol concepts.

Contract (docs/language/static-semantics.md 3.5 "Initialization And
Conversion", 3.9.2 "Conversion Operators", the default-construction rules,
and docs/language/concepts.md "Protocol concepts and conformance blocks"):
an `implicit` constructor converts a declaration initializer or a declared
value return; an `implicit operator T()` converts the same two contexts, and
`T(value)` calls any conversion operator. A conversion is a member call on
the converted value: a read-only receiver selects the read-only operator, a
mutable place prefers a `mut` operator, a temporary or `std::move` prefers a
consuming one, and a temporary receiver ends with its full-expression.
Operators are inherited and may be virtual. Default construction is a
declared API (`T() = default`, a body, or all-default parameters), and a
bare `return;` uses an `implicit` zero-argument constructor. A protocol
concept call `C<T>::op(value)` runs the operation of the conformance block
for exactly that `T`, including blocks generic over `Box<T>` that delegate
to `C<T>`.

Each cell prints a trace of destructions and conversion calls, derived from
these rules, and finishes with `ok`.

Axes: conversion path (implicit constructor, implicit and explicit
operators, consuming and mutable receivers, virtual and inherited
operators, bool) x context (declaration, return, explicit call, generic
body, lambda return, container element, ternary arm); default construction
form x caller (direct, braces, generic factory, bare return, field);
protocol concept shape (direct, generic delegation, associated type,
multi-parameter) x call site (direct, generic function, lambda, loop).

Excluded shapes: conversions in arguments, assignment, operator operands and
`auto` (the language never converts there); conversion chains (never
searched); custom copy or move constructor bodies (not supported).
"""

PRELUDE = """import std.format;
import std.vector;
import std.string;
import std.utility;
void note(int32_t code) { std::println("{}", code); }
class Mark {
  int32_t code;
public:
  Mark(int32_t code) : code(code) {}
  Mark(Mark& other) = delete;
  Mark(Mark&& other) = default;
  ~Mark() { note(this.code); }
  int32_t value() { return this.code; }
};
class Celsius {
public:
  int32_t tenths;
  implicit Celsius(int32_t degrees) : tenths(degrees * 10) {}
};
class Wrapper {
public:
  Mark mark;
  implicit Wrapper(Mark mark) : mark(std::move(mark)) {}
};
class Ticket {
public:
  Mark mark;
  Ticket(Mark mark) : mark(std::move(mark)) {}
};
class Token {
  mut Mark mark;
  int32_t extra;
public:
  Token(int32_t code) : mark(Mark(code)), extra(0) {}
  implicit operator int32_t() { return this.mark.value() + 100; }
  implicit operator Ticket() && { return Ticket(std::move(this.mark)); }
};
struct Counter {
  mut int32_t calls = 0;
  operator int32_t() mut { this.calls += 1; return this.calls; }
  operator int64_t() { return int64_t(-1); }
public:
  Counter() = default;
};
class Shape {
public:
  Shape() = default;
  virtual int32_t sides() { return 1; }
  operator int32_t() { return this.sides(); }
  implicit operator bool() { return true; }
};
class Square : public Shape {
public:
  Square() = default;
  int32_t sides() override { return 4; }
};
class Tile : public Square {
public:
  Tile() = default;
};
class Holder<T> {
public:
  T value;
  Holder(T value) : value(value) {}
  implicit operator T() { return this.value; }
};
T unwrap<T>(Holder<T>& holder) {
  T result = holder;
  return result;
}
"""

CELLS = {
    "implicit_ctor_declaration": ("""  Celsius warm = 21;
  Celsius cold = Celsius(3);
  if (warm.tenths + cold.tenths != 240) return 1;""", []),
    "implicit_ctor_return": ("""  auto read = []() -> Celsius { return 5; };
  Celsius from_lambda = read();
  Celsius from_function = celsius_of(7);
  if (from_lambda.tenths + from_function.tenths != 120) return 1;""", []),
    "implicit_ctor_owner": ("""  {
    Wrapper held = Mark(6);
    note(50);
    Wrapper returned = wrap(7);
    note(51);
    if (held.mark.value() + returned.mark.value() != 13) return 1;
  }""", [50, 51, 7, 6]),
    "operator_declaration": ("""  {
    Token token = Token(3);
    int32_t viewed = token;
    int32_t explicit_view = int32_t(token);
    note(50);
    if (viewed + explicit_view != 206) return 1;
  }""", [50, 3]),
    "operator_temporary_receiver": ("""  int32_t viewed = int32_t(Token(4));
  note(50);
  int32_t declared = Token(5);
  note(51);
  if (viewed + declared != 209) return 1;""", [4, 50, 5, 51]),
    "consuming_operator": ("""  {
    mut Token token = Token(8);
    Ticket moved = Ticket(std::move(token));
    note(50);
    Ticket fresh = Token(9);
    note(51);
    if (moved.mark.value() + fresh.mark.value() != 17) return 1;
  }""", [50, 51, 9, 8]),
    "mutable_operator": ("""  mut Counter counter{};
  int32_t first = int32_t(counter);
  int32_t second = int32_t(counter);
  Counter fixed{};
  int64_t other = int64_t(fixed);
  if (first != 1 || second != 2 || counter.calls != 2 || other != int64_t(-1)) return 1;""", []),
    "virtual_operator": ("""  Square square{};
  Tile tile{};
  Shape plain{};
  Shape& as_base = square;
  Shape& tile_base = tile;
  if (int32_t(as_base) != 4 || int32_t(tile_base) != 4 || int32_t(plain) != 1) return 1;
  bool ready = tile;
  if (!ready || !bool(as_base)) return 2;""", []),
    # static-semantics.md 3.9.2: a conversion operator "may be constexpr or
    # virtual" and is overridden like any member.
    "virtual_operator_declared": ("""  Dial dial{};
  Knob knob{};
  Dial& as_base = knob;
  if (int32_t(dial) != 1 || int32_t(as_base) != 2) return 1;""", [],
                                  """class Dial {
public:
  Dial() = default;
  virtual operator int32_t() { return 1; }
};
class Knob : public Dial {
public:
  Knob() = default;
  operator int32_t() override { return 2; }
};
"""),
    "generic_conversion": ("""  Token token = Token(2);
  Holder<int32_t> number = Holder<int32_t>(102);
  int32_t viewed = unwrap(number);
  Holder<int64_t> wide = Holder<int64_t>(int64_t(7));
  int64_t widened = unwrap(wide);
  note(50);
  if (viewed != 102 || widened != int64_t(7)) return 1;""", [50, 2]),
    "container_elements": ("""  {
    mut std::vector<Ticket> tickets{};
    tickets.push_back(Ticket(Token(1)));
    tickets.push_back(Ticket(Mark(2)));
    note(50);
    mut int32_t sum = 0;
    for (auto& ticket : tickets) sum += ticket.mark.value();
    if (sum != 3) return 1;
    tickets.pop_back();
    note(51);
  }""", [50, 2, 51, 1]),
    "ternary_arms": ("""  Token small = Token(1);
  Token large = Token(2);
  bool pick = int32_t(small) == 101;
  int32_t chosen = pick ? int32_t(large) : int32_t(small);
  note(50);
  if (chosen != 102) return 1;""", [50, 2, 1]),
    "default_forms": ("""  Record braces{};
  Record call = Record();
  Record made = make<Record>();
  Defaulted argument{};
  Defaulted explicit_value = Defaulted(9);
  Implicit bare = empty_implicit();
  Outer outer{};
  if (braces.value + call.value + made.value != 15) return 1;
  if (argument.value != 3 || explicit_value.value != 9) return 2;
  if (bare.value != 11 || outer.inner.value != 5 || outer.count != 2) return 3;""", []),
    "default_owner_fields": ("""  {
    Owning owning{};
    note(50);
    Owning other = make<Owning>();
    note(51);
  }""", [50, 51, 22, 21, 22, 21]),
    "protocol_direct": ("""  Unit unit = Unit(4);
  Box<Unit> boxed = Box<Unit>(Unit(5));
  Box<Box<Unit>> nested = Box<Box<Unit>>(Box<Unit>(Unit(6)));
  if (read_score(unit) != 4 || read_score(boxed) != 10 || read_score(nested) != 24) return 1;""", []),
    "protocol_loop": ("""  mut std::vector<Box<Unit>> boxes{};
  for (mut int32_t i = 1; i <= 4; i += 1) boxes.push_back(Box<Unit>(Unit(i)));
  if (total_score(boxes) != 20) return 1;
  auto scored = [](Box<Unit>& value) -> int32_t { return score<Box<Unit>>::read(value); };
  if (scored(boxes[3]) != 8) return 2;""", []),
    "protocol_associated": ("""  Meters m = Meters(7);
  Feet f = Feet(10);
  if (doubled(m) != 14 || doubled(f) != 6) return 1;
  if (measure<Meters>::unit_code() != 1 || measure<Feet>::unit_code() != 2) return 2;""", []),
    "protocol_multi_parameter": ("""  Meters m = Meters(7);
  Feet f = Feet(10);
  if (convert_to<Feet, Meters>::convert(m).value != 21) return 1;
  if (convert_to<Meters, Feet>::convert(f).value != 3) return 2;""", []),
}

HELPERS = """Celsius celsius_of(int32_t degrees) { return degrees; }
Wrapper wrap(int32_t code) { return Mark(code); }
struct Record {
  int32_t value = 5;
  Record() = default;
};
struct Defaulted {
  int32_t value;
  Defaulted(int32_t input = 3) : value(input) {}
};
struct Implicit {
  int32_t value;
  implicit Implicit() : value(11) {}
};
Implicit empty_implicit() { return; }
struct Outer {
  Record inner;
  int32_t count = 2;
  Outer() = default;
};
class Owning {
public:
  Mark first = Mark(21);
  Mark second = Mark(22);
  Owning() = default;
};
T make<std::default_initializable T>() { return T(); }
concept score<T> { int32_t read(T& value); };
struct Unit {
  int32_t value;
  Unit(int32_t value) : value(value) {}
};
struct Box<T> {
  T value;
  Box(T value) : value(value) {}
};
for concept score<Unit> { int32_t read(Unit& value) { return value.value; } };
for <T> concept score<Box<T>> requires score<T> {
  int32_t read(Box<T>& value) { return score<T>::read(value.value) * 2; }
};
int32_t read_score<T>(T& value) requires score<T> { return score<T>::read(value); }
int32_t total_score<T>(std::vector<T>& values) requires score<T> && std::vector_element<T> {
  mut int32_t sum = 0;
  for (auto& value : values) sum += score<T>::read(value);
  return sum;
}
struct Meters { int32_t value; Meters(int32_t value) : value(value) {} };
struct Feet { int32_t value; Feet(int32_t value) : value(value) {} };
concept measure<T> {
  typename amount;
  amount amount_of(T& value);
  int32_t unit_code();
};
for concept measure<Meters> {
  using amount = int32_t;
  int32_t amount_of(Meters& value) { return value.value; }
  int32_t unit_code() { return 1; }
};
for concept measure<Feet> {
  using amount = int32_t;
  int32_t amount_of(Feet& value) { return value.value * 3 / 10; }
  int32_t unit_code() { return 2; }
};
int32_t doubled<T>(T& value) requires measure<T> && std::integral<measure<T>::amount> {
  measure<T>::amount amount = measure<T>::amount_of(value);
  return amount * 2;
}
concept convert_to<To, From> { To convert(From& value); };
for concept convert_to<Feet, Meters> { Feet convert(Meters& value) { return Feet(value.value * 3); } };
for concept convert_to<Meters, Feet> { Meters convert(Feet& value) { return Meters(value.value * 3 / 10); } };
"""

MAIN = """int32_t run() {
BODY
  return 0;
}
int main() {
  int32_t status = run();
  if (status != 0) {
    std::println("status {}", status);
    return status;
  }
  std::println("ok");
  return 0;
}
"""


def cells():
    for name, cell in CELLS.items():
        body, trace = cell[0], cell[1]
        extra = cell[2] if len(cell) > 2 else ""
        stdout = "".join(f"{code}\n" for code in trace) + "ok\n"
        yield (name, PRELUDE + HELPERS + extra + MAIN.replace("BODY", body),
               {"stdout": stdout})


# A deliberately broken variant the oracle must flag (the receiver outlives its full-expression):
# cell, original text, replacement. `--negative-controls` builds it.
NEGATIVE_CONTROL = ('operator_temporary_receiver',
                    'int32_t viewed = int32_t(Token(4));',
                    'Token held = Token(4);\n  int32_t viewed = int32_t(held);')
