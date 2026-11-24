"""Matrix: `?` propagation and error conversion through owners and cleanup.

Contract (docs/language/execution.md 4.8, docs/language/expected.md
"Propagation"): `?operand` evaluates its operand once. On success it yields
the value and evaluation continues; on failure it consumes the error, converts
it when the enclosing return type needs it (`std::from<F, E>` for
`expected`, the return type's implicit residual constructor or `std::from<R,
Residual>` for a `std::tryable` carrier), then destroys the full-expression's
earlier temporaries in reverse construction order and performs the ordinary
lexical return cleanup. Nothing after the failing operator is evaluated. A
moved error payload is destroyed exactly once, with the caller's result.

Every Mark prints its code when destroyed and a conversion prints 80, so the
expected stdout of each cell is derived from those rules: the success trace,
90, the failure trace (conversion first, then temporaries, then locals), the
failed result's payload when the caller drops it, 91, then `ok`.

Axes: carrier (expected with the same error, expected converted with
`std::from`, a move-only error payload converted by move, a user
`std::tryable` with an implicit residual constructor, a tryable converted
with `std::from`, `std::optional`) x position of `?` (after locals, inside a
full-expression with temporaries, in a loop, in an init statement, in a
constructor argument, through a nested call, in a lambda, in a switch arm).

Excluded shapes: conversion chains (the language never searches them);
`?` in a constant context (the constexpr oracle's job); reference payloads
(`expected<T&, E>`, covered by borrowed_elements and lifetimes).
"""

PRELUDE = """import std.format;
import std.optional;
import std.utility;
class Mark {
  int32_t code;
public:
  Mark(int32_t code) : code(code) {}
  Mark(Mark& other) = delete;
  Mark(Mark&& other) = default;
  ~Mark() { std::println("{}", this.code); }
  int32_t value() { return this.code; }
};
int32_t combine(int32_t a, int32_t b) { return a + b; }
enum class Kind { pos, neg };
Kind kind(int32_t raw) { return raw > 0 ? Kind::pos : Kind::neg; }
"""

# carrier: (declarations defining `SRC source(int32_t raw)` that fails with
# code -raw when raw < 0, the return type RET, the success return expression
# OK(x), a check `good(result, x)` and `failed(result, code)` as bool
# functions, conversion trace, failed-result drop trace)
CARRIERS = {
    "expected_same": ("""enum Fault { failed(int32_t code) };
expected<int32_t, Fault> source(int32_t raw) {
  if (raw < 0) return unexpected(Fault::failed(-raw));
  return raw;
}
bool good(expected<int32_t, Fault>& result, int32_t x) { return result.has_value() && result.value() == x; }
bool failed(expected<int32_t, Fault>& result, int32_t code) {
  if (result.has_value()) return false;
  switch (result.error()) {
    case Fault::failed(n): return n == code;
  }
}
""", "expected<int32_t, Fault>", "X", [], []),
    "expected_from": ("""enum Low { failed(int32_t code) };
enum High { wrapped(int32_t code) };
for concept std::from<High, Low> {
  High from(Low error) {
    std::println("{}", 80);
    switch (error) {
      case Low::failed(n): return High::wrapped(n);
    }
  }
};
expected<int32_t, Low> source(int32_t raw) {
  if (raw < 0) return unexpected(Low::failed(-raw));
  return raw;
}
bool good(expected<int32_t, High>& result, int32_t x) { return result.has_value() && result.value() == x; }
bool failed(expected<int32_t, High>& result, int32_t code) {
  if (result.has_value()) return false;
  switch (result.error()) {
    case High::wrapped(n): return n == code;
  }
}
""", "expected<int32_t, High>", "X", [80], []),
    "owned_error": ("""enum Low { failed(Mark mark) };
enum High { wrapped(Mark mark) };
for concept std::from<High, Low> {
  High from(Low error) {
    std::println("{}", 80);
    switch (std::move(error)) {
      case Low::failed(mark): return High::wrapped(std::move(mark));
    }
  }
};
expected<int32_t, Low> source(int32_t raw) {
  if (raw < 0) return unexpected(Low::failed(Mark(40 - raw)));
  return raw;
}
bool good(expected<int32_t, High>& result, int32_t x) { return result.has_value() && result.value() == x; }
bool failed(expected<int32_t, High>& result, int32_t code) {
  if (result.has_value()) return false;
  switch (result.error()) {
    case High::wrapped(auto& mark): return mark.value() == 40 + code;
  }
}
""", "expected<int32_t, High>", "X", [80], [44]),
    "tryable": ("""struct Rejected {
  int32_t code;
  Rejected(int32_t code) : code(code) {}
};
struct Checked {
  int32_t value;
  int32_t code;
  Checked(int32_t value, int32_t code) : value(value), code(code) {}
};
for concept std::tryable<Checked> {
  using value_type = int32_t;
  using residual_type = Rejected;
  bool has_value(Checked& result) { return result.code == 0; }
  int32_t take_value(Checked result) { return result.value; }
  Rejected take_residual(Checked result) { return Rejected(result.code); }
};
class Status {
public:
  int32_t value;
  int32_t code;
  Status(int32_t value, int32_t code) : value(value), code(code) {}
  implicit Status(Rejected rejected) : value(0), code(rejected.code) {}
};
Checked source(int32_t raw) {
  if (raw < 0) return Checked(0, -raw);
  return Checked(raw, 0);
}
bool good(Status& result, int32_t x) { return result.code == 0 && result.value == x; }
bool failed(Status& result, int32_t code) { return result.code == code; }
""", "Status", "Status(X, 0)", [], []),
    "tryable_from": ("""struct Rejected {
  int32_t code;
  Rejected(int32_t code) : code(code) {}
};
struct Checked {
  int32_t value;
  int32_t code;
  Checked(int32_t value, int32_t code) : value(value), code(code) {}
};
for concept std::tryable<Checked> {
  using value_type = int32_t;
  using residual_type = Rejected;
  bool has_value(Checked& result) { return result.code == 0; }
  int32_t take_value(Checked result) { return result.value; }
  Rejected take_residual(Checked result) { return Rejected(result.code); }
};
enum Outcome { value(int32_t n), failure(int32_t n) };
for concept std::from<Outcome, Rejected> {
  Outcome from(Rejected error) {
    std::println("{}", 80);
    return Outcome::failure(error.code);
  }
};
Checked source(int32_t raw) {
  if (raw < 0) return Checked(0, -raw);
  return Checked(raw, 0);
}
bool good(Outcome& result, int32_t x) {
  switch (result) {
    case Outcome::value(n): return n == x;
    case Outcome::failure(n): return false;
  }
}
bool failed(Outcome& result, int32_t code) {
  switch (result) {
    case Outcome::value(n): return false;
    case Outcome::failure(n): return n == code;
  }
}
""", "Outcome", "Outcome::value(X)", [80], []),
    "optional": ("""std::optional<int32_t> source(int32_t raw) {
  if (raw < 0) return std::optional<int32_t>();
  return std::optional<int32_t>(raw);
}
bool good(std::optional<int32_t>& result, int32_t x) { return result.has_value() && result.value() == x; }
bool failed(std::optional<int32_t>& result, int32_t code) { return !result.has_value(); }
""", "std::optional<int32_t>", "std::optional<int32_t>(X)", [], []),
}

# position: (step functions returning RET, success value for step(5),
# success trace, failure trace after conversion for step(-4))
POSITIONS = {
    "locals": ("""RET step(int32_t raw) {
  Mark a = Mark(1);
  int32_t v = ?source(raw);
  Mark b = Mark(2);
  return OK(v);
}
""", 5, [2, 1], [1]),
    "temporary": ("""RET step(int32_t raw) {
  Mark a = Mark(1);
  int32_t v = Mark(3).value() + ?source(raw) + Mark(4).value();
  return OK(v);
}
""", 12, [4, 3, 1], [3, 1]),
    "loop": ("""RET step(int32_t raw) {
  Mark a = Mark(1);
  mut int32_t sum = 0;
  for (mut int32_t i = 1; i <= 3; i += 1) {
    Mark s = Mark(10 + i);
    sum += ?source(i == 2 ? raw : i);
  }
  return OK(sum);
}
""", 9, [11, 12, 13, 1], "loop"),
    "init_statement": ("""RET step(int32_t raw) {
  Mark a = Mark(1);
  if (int32_t v = ?source(raw); v > 0) {
    Mark b = Mark(2);
    return OK(v);
  }
  return OK(0);
}
""", 5, [2, 1], [1]),
    "argument": ("""RET step(int32_t raw) {
  Mark a = Mark(1);
  Mark b = Mark(combine(Mark(5).value(), ?source(raw)));
  return OK(b.value());
}
""", 10, [5, 10, 1], [5, 1]),
    "nested": ("""RET inner(int32_t raw) {
  Mark m = Mark(7);
  int32_t v = ?source(raw);
  return OK(v);
}
RET step(int32_t raw) {
  Mark a = Mark(1);
  RET r = inner(raw);
  if (!good(r, raw)) return r;
  return OK(raw + 1);
}
""", 6, [7, 1], [7, 1]),
    "lambda": ("""RET step(int32_t raw) {
  Mark a = Mark(1);
  auto body = [raw]() -> RET {
    Mark m = Mark(6);
    int32_t v = ?source(raw);
    return OK(v * 3);
  };
  return body();
}
""", 15, [6, 1], [6, 1]),
    "switch_arm": ("""RET step(int32_t raw) {
  Mark a = Mark(1);
  switch (kind(raw)) {
  case Kind::pos: {
    Mark b = Mark(2);
    int32_t v = ?source(raw);
    return OK(v);
  }
  case Kind::neg: {
    Mark c = Mark(3);
    int32_t v = ?source(raw);
    return OK(v);
  }
  }
  return OK(0);
}
""", 5, [2, 1], [3, 1]),
}

MAIN = """int32_t run() {
  {
    auto result = step(5);
    if (!good(result, SUCCESS)) return 1;
  }
  std::println("{}", 90);
  {
    auto result = step(-4);
    if (!failed(result, 4)) return 2;
  }
  std::println("{}", 91);
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


def trace(carrier, position):
    _, _, _, converted, dropped = CARRIERS[carrier]
    _, _, success, failure = POSITIONS[position]
    if failure == "loop":
        # The first iteration completes; the second fails inside its body.
        failure_trace = [11] + converted + [12, 1]
    else:
        failure_trace = converted + failure
    return success + [90] + failure_trace + dropped + [91]


def program(carrier, position):
    declarations, ret, ok, _, _ = CARRIERS[carrier]
    body, value, _, _ = POSITIONS[position]
    step = body.replace("RET", ret)
    # OK(expr) -> the carrier's success form.
    out = []
    rest = step
    while "OK(" in rest:
        start = rest.index("OK(")
        depth, end = 0, start + 2
        while True:
            if rest[end] == "(":
                depth += 1
            elif rest[end] == ")":
                depth -= 1
                if depth == 0:
                    break
            end += 1
        out.append(rest[:start] + ok.replace("X", rest[start + 3:end]))
        rest = rest[end + 1:]
    step = "".join(out) + rest
    return PRELUDE + declarations + step + MAIN.replace("SUCCESS", str(value))


# A tryable whose operand owns a Mark and whose success value is an owner:
# the consumed operand is destroyed when take_value or take_residual returns
# (its by-value parameter), the success owner at the end of its iteration.
SCENARIOS = {
    "owning_tryable_loop": ("""void note(int32_t code) { std::println("{}", code); }
struct Rejected { int32_t code; Rejected(int32_t code) : code(code) {} };
class Checked {
public:
  Mark tag;
  int32_t value;
  int32_t code;
  Checked(int32_t id, int32_t value, int32_t code) : tag(Mark(id)), value(value), code(code) {}
};
for concept std::tryable<Checked> {
  using value_type = Mark;
  using residual_type = Rejected;
  bool has_value(Checked& c) { return c.code == 0; }
  Mark take_value(Checked c) { note(70); return Mark(c.value); }
  Rejected take_residual(Checked c) { note(71); return Rejected(c.code); }
};
class Status {
public:
  int32_t code;
  Status(int32_t code) : code(code) {}
  implicit Status(Rejected r) : code(r.code) {}
};
Checked make(int32_t id, int32_t value, int32_t code) { return Checked(id, value, code); }
Status run(int32_t limit) {
  Mark guard = Mark(1);
  mut int32_t total = 0;
  for (mut int32_t i = 1; i <= 3; i += 1) {
    Mark got = ?make(10 + i, 20 + i, i == limit ? 5 : 0);
    total += got.value();
  }
  note(total);
  return Status(0);
}
int main() {
  Status a = run(9);
  note(90);
  Status b = run(2);
  note(91);
  if (a.code != 0 || b.code != 5) return 1;
  std::println("ok");
  return 0;
}
""", [70, 11, 21, 70, 12, 22, 70, 13, 23, 66, 1, 90,
                             70, 11, 21, 71, 12, 1, 91]),
}


def cells():
    for carrier in CARRIERS:
        for position in POSITIONS:
            text = program(carrier, position)
            stdout = "".join(f"{code}\n" for code in trace(carrier, position))
            yield f"{carrier}_{position}", text, {"stdout": stdout + "ok\n"}
    for name, (text, expected) in SCENARIOS.items():
        stdout = "".join(f"{code}\n" for code in expected)
        yield name, PRELUDE + text, {"stdout": stdout + "ok\n"}


# A deliberately broken variant the oracle must flag (the temporary is created after the failing `?`):
# cell, original text, replacement. `--negative-controls` builds it.
NEGATIVE_CONTROL = ('expected_from_temporary',
                    'Mark(3).value() + ?source(raw)',
                    '?source(raw) + Mark(3).value()')
