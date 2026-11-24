#!/usr/bin/env python3
"""Compile-time and run-time evaluation of the same function agree.

Every cell defines `constexpr` functions and evaluates each one twice: in a
`constexpr` binding, which the frontend's verified-MIR constant evaluator
computes, and in an ordinary call whose arguments pass through an opaque
helper (an atomic load), which the LLVM backend compiles. The program checks
that both values match and prints `ok`. This is the RFC 0013 compile-time and
run-time agreement oracle, aimed at `mir_constant_evaluator.cpp` and the
checked-arithmetic lowering.

Families:

  arith/<type>/<n>   random checked-integer expression trees over int32_t,
                     int64_t, uint32_t and uint64_t. A Python model of
                     docs/language/execution.md (checked +, -, *, truncating
                     /, %, two's-complement bitwise operations, checked
                     shifts, checked negation) gives a third, independent
                     expected value. An expression the model says must fail
                     becomes a pair of cells: `.../ct` must be rejected at
                     compile time and `.../rt` must stop with the matching
                     GTI-R runtime failure code.
  small/<type>/<n>   the same trees over int8_t, int16_t, uint8_t and
                     uint16_t, with no model (their promotion rules are not
                     specified in docs/language). Compile-time and run-time
                     results must agree; a compile-time failure must be
                     matched by a runtime failure.
  shapes/<name>      hand-written constexpr programs over loops, recursion,
                     fixed arrays, records with constexpr constructors and
                     methods, scoped enums and switch, expected and `?`,
                     mutable references, string views, user operators and
                     floating point.

Excluded: formatted panics and owning text (outside the constant profile per
static-semantics.md 3.11), payload-enum execution, and NaN results (NaN
compares unequal to itself, so agreement is checked on non-NaN values only).

Negative control: `--control` builds one cell whose run-time side adds one,
which must report WRONG.
"""
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import matrix_support  # noqa: E402

TYPES = {
    "int32_t": (32, True), "int64_t": (64, True),
    "uint32_t": (32, False), "uint64_t": (64, False),
}
SMALL = {
    "int8_t": (8, True), "int16_t": (16, True),
    "uint8_t": (8, False), "uint16_t": (16, False),
}
FAILURE_CODE = {
    "overflow": "GTI-R0001", "div0": "GTI-R0002", "mod0": "GTI-R0003",
    "negshift": "GTI-R0004", "bigshift": "GTI-R0005",
    "convert": "GTI-R0006",
}

PRELUDE = """import std.atomic;
import std.format;
import std.panic;
import std.functional;
std::atomic<int32_t> zero{0};
T opaque<T>(T value) {
  if (zero.load() != 0) { std::panic("opaque"); }
  return value;
}
"""


class Fail(Exception):
    def __init__(self, kind):
        self.kind = kind


def bounds(bits, signed):
    if signed:
        return -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    return 0, (1 << bits) - 1


def checked(value, bits, signed):
    low, high = bounds(bits, signed)
    if not low <= value <= high:
        raise Fail("overflow")
    return value


def wrap(value, bits, signed):
    value &= (1 << bits) - 1
    if signed and value >= 1 << (bits - 1):
        value -= 1 << bits
    return value


def evaluate(node, env, bits, signed):
    kind = node[0]
    if kind == "var":
        return env[node[1]]
    if kind == "lit":
        return node[1]
    if kind == "neg":
        return checked(-evaluate(node[1], env, bits, signed), bits, signed)
    op, left, right = node[1], node[2], node[3]
    a = evaluate(left, env, bits, signed)
    b = evaluate(right, env, bits, signed)
    if op == "+":
        return checked(a + b, bits, signed)
    if op == "-":
        return checked(a - b, bits, signed)
    if op == "*":
        return checked(a * b, bits, signed)
    if op == "/":
        if b == 0:
            raise Fail("div0")
        quotient = abs(a) // abs(b)
        return checked(quotient if (a < 0) == (b < 0) else -quotient,
                       bits, signed)
    if op == "%":
        if b == 0:
            raise Fail("mod0")
        remainder = abs(a) % abs(b)
        return -remainder if a < 0 else remainder
    if op in "&|^":
        mask = (1 << bits) - 1
        ua, ub = a & mask, b & mask
        result = ua & ub if op == "&" else ua | ub if op == "|" else ua ^ ub
        return wrap(result, bits, signed)
    if op in ("<<", ">>"):
        if b < 0:
            raise Fail("negshift")
        if b >= bits:
            raise Fail("bigshift")
        if op == "<<":
            return wrap(a << b, bits, signed)
        return a >> b  # Python's >> is arithmetic for negative values
    raise AssertionError(op)


def literal(type_name, value, bits, signed):
    """A literal with the type's exact suffix, valid even at the minimum."""
    suffix = f"{'i' if signed else 'u'}{bits}"
    low, _ = bounds(bits, signed)
    if signed and value == low:
        text = f"(-{-(value + 1)}{suffix} - 1{suffix})"
        # Narrow operands promote, so the minimum is converted back.
        return f"{type_name}{text}" if bits < 32 else text
    if value < 0:
        return f"(-{-value}{suffix})"
    return f"{value}{suffix}"


def render(node, type_name, bits, signed):
    kind = node[0]
    if kind == "var":
        return node[1]
    if kind == "lit":
        return literal(type_name, node[1], bits, signed)
    if kind == "neg":
        return f"(-{render(node[1], type_name, bits, signed)})"
    return (f"({render(node[2], type_name, bits, signed)} {node[1]} "
            f"{render(node[3], type_name, bits, signed)})")


def has_variable(node):
    if node[0] == "var":
        return True
    return any(has_variable(child) for child in node[1:] if isinstance(child, tuple))


def tree(rng, depth, bits, signed):
    if depth == 0 or rng.random() < 0.25:
        if rng.random() < 0.7:
            return ("var", rng.choice("abc"))
        low, high = bounds(bits, signed)
        value = rng.choice([1, 2, 3, 7, 31, high, bits - 1,
                            rng.randint(1, 1000)])
        if signed and rng.random() < 0.3:
            value = -value
        return ("lit", max(low, min(high, value)))
    if signed and rng.random() < 0.08:
        return ("neg", tree(rng, depth - 1, bits, signed))
    op = rng.choice(["+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>",
                     "+", "-", "*"])
    left = tree(rng, depth - 1, bits, signed)
    right = tree(rng, depth - 1, bits, signed)
    if op in ("/", "%", "<<", ">>") and not has_variable(right):
        # A constant divisor or shift count is checked by semantics, not
        # evaluated (execution.md 4.3), so keep these operands dynamic.
        right = ("var", rng.choice("abc"))
    return ("bin", op, left, right)


def inputs(rng, bits, signed):
    low, high = bounds(bits, signed)
    edges = [0, high, high - 1] + ([low, low + 1, -1] if signed else [])
    def pick():
        if rng.random() < 0.15:
            return rng.choice(edges)
        value = rng.randint(0, min(high, 1 << rng.choice([2, 4, 8, 12])))
        return -value if signed and rng.random() < 0.4 else value
    return {name: pick() for name in "abc"}


def function(name, type_name, expression):
    # Arithmetic on 8- and 16-bit operands promotes; the explicit checked
    # conversion back to the declared type is part of the expression.
    if type_name in SMALL:
        expression = f"{type_name}({expression})"
    return (f"constexpr {type_name} {name}({type_name} a, {type_name} b, "
            f"{type_name} c) {{\n  return {expression};\n}}\n")


def call(name, type_name, env, bits, signed, runtime):
    args = [literal(type_name, env[v], bits, signed) for v in "abc"]
    if runtime:
        args = [f"opaque({arg})" for arg in args]
    return f"{name}({', '.join(args)})"


def arith_cells(rng, count, types, modelled):
    """(key, source, options) for random expression cells."""
    cells = []
    for type_name, (bits, signed) in types.items():
        passing = []
        for index in range(count):
            node = tree(rng, 3, bits, signed)
            env = inputs(rng, bits, signed)
            expression = render(node, type_name, bits, signed)
            try:
                if type_name in SMALL:
                    # Promotion model: 8- and 16-bit operands promote to
                    # int32_t, the operation is checked there, and the
                    # explicit conversion back to the type is checked.
                    expected = evaluate(node, env, 32, True)
                    low, high = bounds(bits, signed)
                    if not low <= expected <= high:
                        raise Fail("convert")
                else:
                    expected = evaluate(node, env, bits, signed)
                failure = None
            except Fail as error:
                failure = error.kind
            name = f"e{index}"
            if failure is None or not modelled:
                passing.append((name, expression, env, expected if failure is None else None))
                continue
            text = function(name, type_name, expression)
            ct = (PRELUDE + text + f"constexpr {type_name} value = "
                  f"{call(name, type_name, env, bits, signed, False)};\n"
                  "int main() {\n  std::println(\"ok\");\n  return 0;\n}\n")
            rt = (PRELUDE + text + "int main() {\n"
                  f"  {type_name} value = "
                  f"{call(name, type_name, env, bits, signed, True)};\n"
                  "  std::println(\"unreachable {}\", value);\n  return 0;\n}\n")
            key = f"{'small' if type_name in SMALL else 'arith'}/{type_name}/{index}"
            cells.append((key + "/ct", ct, {"expect_build_error": r"error\[GTI-",
                                           "execute": False}))
            cells.append((key + "/rt", rt, {"expect_run_error": FAILURE_CODE[failure]}))
        # Group passing expressions eight to a cell to bound compile count.
        for start in range(0, len(passing), 8):
            group = passing[start:start + 8]
            body = "".join(function(name, type_name, expression)
                           for name, expression, _, _ in group)
            checks = []
            for name, _, env, expected in group:
                ct = call(name, type_name, env, bits, signed, False)
                rt = call(name, type_name, env, bits, signed, True)
                checks.append(f"  constexpr {type_name} {name}_ct = {ct};\n"
                              f"  {type_name} {name}_rt = {rt};\n"
                              f"  if ({name}_ct != {name}_rt) {{ std::println("
                              f"\"{name} ct {{}} rt {{}}\", {name}_ct, {name}_rt); return 1; }}\n")
                if expected is not None and modelled:
                    checks.append(f"  if ({name}_rt != {literal(type_name, expected, bits, signed)}) "
                                  f"{{ std::println(\"{name} model {expected} got {{}}\", {name}_rt); return 1; }}\n")
            source = (PRELUDE + body + "int main() {\n" + "".join(checks)
                      + "  std::println(\"ok\");\n  return 0;\n}\n")
            family = "small" if type_name in SMALL else "arith"
            cells.append((f"{family}/{type_name}/g{start // 8}", source,
                          {"expect_stdout": "ok\n"}))
    return cells


# Hand-written shapes: (name, declarations, [(type, constexpr-call, runtime-call)]).
# Runtime calls wrap each argument in opaque().
SHAPES = {
    "loops": ("""
constexpr int64_t collatz_steps(int64_t start) {
  mut int64_t value = start;
  mut int64_t steps = 0;
  while (value != 1) {
    if (value % 2 == 0) { value = value / 2; } else { value = 3 * value + 1; }
    steps++;
  }
  return steps;
}
constexpr uint64_t gcd(uint64_t a, uint64_t b) {
  mut uint64_t x = a;
  mut uint64_t y = b;
  while (y != 0u64) { uint64_t t = x % y; x = y; y = t; }
  return x;
}
constexpr int32_t nested(int32_t n) {
  mut int32_t total = 0;
  for (mut int32_t i = 0; i < n; i++) {
    for (mut int32_t j = i; j < n; j++) {
      if ((i + j) % 3 == 0) { continue; }
      if (j > 2 * i + 5) { break; }
      total += i * j - j;
    }
  }
  return total;
}
constexpr int32_t do_loop(int32_t n) {
  mut int32_t k = n;
  mut int32_t acc = 1;
  do { acc = acc * 3 % 1000 + k; k--; } while (k > 0);
  return acc;
}
""", [("int64_t", "collatz_steps({})", ["27i64"]), ("int64_t", "collatz_steps({})", ["97i64"]),
      ("uint64_t", "gcd({}, {})", ["1071u64", "462u64"]), ("uint64_t", "gcd({}, {})", ["0u64", "9u64"]),
      ("int32_t", "nested({})", [0]), ("int32_t", "nested({})", [17]),
      ("int32_t", "do_loop({})", [0]), ("int32_t", "do_loop({})", [9])]),
    "recursion": ("""
constexpr int64_t fib(int32_t n) { if (n < 2) { return int64_t(n); } return fib(n - 1) + fib(n - 2); }
constexpr int32_t ackermann(int32_t m, int32_t n) {
  if (m == 0) { return n + 1; }
  if (n == 0) { return ackermann(m - 1, 1); }
  return ackermann(m - 1, ackermann(m, n - 1));
}
constexpr bool is_even(uint32_t n);
constexpr bool is_odd(uint32_t n) { if (n == 0u32) { return false; } return is_even(n - 1u32); }
constexpr bool is_even(uint32_t n) { if (n == 0u32) { return true; } return is_odd(n - 1u32); }
""", [("int64_t", "fib({})", [20]), ("int32_t", "ackermann({}, {})", [2, 3]),
      ("bool", "is_odd({})", ["31u32"]), ("bool", "is_even({})", ["40u32"])]),
    "arrays": ("""
constexpr int32_t sorted_signature(int32_t seed) {
  mut int32_t values[8] = {};
  for (mut int32_t i = 0; i < 8; i++) { values[i] = (seed * (i + 3) + 17) % 23 - 11; }
  for (mut int32_t i = 0; i < 8; i++) {
    for (mut int32_t j = 0; j + 1 < 8 - i; j++) {
      if (values[j] > values[j + 1]) { int32_t t = values[j]; values[j] = values[j + 1]; values[j + 1] = t; }
    }
  }
  mut int32_t signature = 0;
  for (mut int32_t i = 0; i < 8; i++) { signature = signature * 7 + values[i]; }
  return signature;
}
constexpr int32_t grid(int32_t seed) {
  mut int32_t cells[3][4] = {};
  for (mut int32_t r = 0; r < 3; r++) {
    for (mut int32_t c = 0; c < 4; c++) { cells[r][c] = seed + r * 10 + c; }
  }
  mut int32_t copy[3][4] = cells;
  copy[1][2] += 100;
  cells[0][0] *= 2;
  return cells[0][0] + cells[1][2] + copy[1][2] + copy[0][0];
}
constexpr int32_t prefix(int32_t seed) {
  mut int64_t sums[6] = {};
  mut int64_t running = 0;
  for (mut int32_t i = 0; i < 6; i++) { running += int64_t(seed - i * i); sums[i] = running; }
  return int32_t(sums[5] - sums[2]);
}
""", [("int32_t", "sorted_signature({})", [5]), ("int32_t", "sorted_signature({})", [-9]),
      ("int32_t", "grid({})", [4]), ("int32_t", "prefix({})", [11])]),
    "records": ("""
struct Vec2 {
  mut int32_t x;
  mut int32_t y;
  constexpr Vec2(int32_t x, int32_t y) : x(x), y(y) {}
  constexpr Vec2 operator+(Vec2 other) { return Vec2(this.x + other.x, this.y + other.y); }
  constexpr int32_t dot(Vec2 other) { return this.x * other.x + this.y * other.y; }
  constexpr void scale(int32_t k) mut { this.x *= k; this.y *= k; }
};
struct Box {
  mut Vec2 low;
  mut Vec2 high;
  mut int32_t tags[3];
  constexpr Box(Vec2 low, Vec2 high) : low(low), high(high), tags({}) {}
};
constexpr int32_t box_area(int32_t seed) {
  mut Box box = Box(Vec2(seed, 1), Vec2(seed + 4, 7));
  box.tags[0] = 1;
  box.tags[2] = 3;
  box.high.scale(2);
  box.tags[1] = box.high.x - box.low.y;
  Vec2 diagonal = box.high + Vec2(-box.low.x, -box.low.y);
  return diagonal.x * diagonal.y + box.tags[0] + box.tags[1] + box.tags[2];
}
constexpr int32_t accumulate(int32_t n) {
  mut Vec2 total = Vec2(0, 0);
  for (mut int32_t i = 1; i <= n; i++) { total = total + Vec2(i, -i * 2); }
  return total.dot(Vec2(3, 1));
}
""", [("int32_t", "box_area({})", [3]), ("int32_t", "accumulate({})", [12])]),
    "enums": ("""
enum class Step : uint8_t { Up, Down, Hold, Reset };
constexpr Step next(Step s, int32_t tick) {
  switch (s) {
    case Step::Up: return tick % 3 == 0 ? Step::Hold : Step::Up;
    case Step::Down: return tick % 5 == 0 ? Step::Reset : Step::Down;
    case Step::Hold: return Step::Down;
    case Step::Reset: return Step::Up;
  }
  return Step::Reset;
}
constexpr int32_t run_machine(int32_t ticks) {
  mut Step state = Step::Up;
  mut int32_t level = 0;
  for (mut int32_t t = 1; t <= ticks; t++) {
    state = next(state, t);
    if (state == Step::Up) { level++; }
    else if (state == Step::Down) { level--; }
    else if (state == Step::Reset) { level = 0; }
  }
  return level * 100 + int32_t(uint8_t(state == Step::Hold ? 1 : 0));
}
""", [("int32_t", "run_machine({})", [0]), ("int32_t", "run_machine({})", [41])]),
    "expected": ("""
constexpr expected<int32_t, int32_t> safe_div(int32_t a, int32_t b) {
  if (b == 0) { return unexpected(a); }
  return a / b;
}
constexpr expected<int32_t, int32_t> chain(int32_t a, int32_t b, int32_t c) {
  int32_t first = ?safe_div(a, b);
  int32_t second = ?safe_div(first, c);
  return first + second;
}
constexpr int32_t settle(int32_t a, int32_t b, int32_t c) {
  expected<int32_t, int32_t> r = chain(a, b, c);
  if (r.has_value()) { return r.value(); }
  return -1000 - r.error();
}
constexpr int32_t fallback(int32_t a, int32_t b) { return safe_div(a, b).value_or(-7); }
""", [("int32_t", "settle({}, {}, {})", [100, 3, 2]), ("int32_t", "settle({}, {}, {})", [100, 0, 2]),
      ("int32_t", "settle({}, {}, {})", [100, 3, 0]), ("int32_t", "fallback({}, {})", [9, 0])]),
    "references": ("""
constexpr void bump(mut int32_t& v, int32_t by) { v += by; }
constexpr mut int32_t& self_ref(mut int32_t& a) { return a; }
struct Pair { mut int32_t left; mut int32_t right;
  constexpr Pair(int32_t l, int32_t r) : left(l), right(r) {}
  constexpr mut int32_t& side(bool l) mut { if (l) { return this.left; } return this.right; } };
constexpr int32_t juggle(int32_t seed) {
  mut int32_t a = seed;
  mut int32_t b = seed * 2;
  bump(self_ref(a), 5);
  bump(self_ref(b), -3);
  mut Pair p = Pair(a, b);
  int32_t tripled = p.side(true) * 3;
  p.side(false) = tripled;
  int32_t right = p.right;
  bump(p.side(true), right);
  mut int32_t values[3] = {1, 2, 3};
  mut int32_t index = 1;
  mut int32_t& slot = values[index];
  index = 2;
  slot += 10;
  return p.left * 1000 + p.right + values[1] * 7 + values[2];
}
""", [("int32_t", "juggle({})", [4]), ("int32_t", "juggle({})", [-6])]),
    "text": ("""
constexpr int32_t vowels(std::string_view s) {
  mut int32_t n = 0;
  for (mut uint64_t i = 0u64; i < s.size(); i++) {
    char ch = s[i];
    if (ch == 'a' || ch == 'e' || ch == 'i' || ch == 'o' || ch == 'u') { n++; }
  }
  return n;
}
constexpr uint64_t hash(std::string_view s) {
  mut uint64_t h = 2166136261u64;
  for (mut uint64_t i = 0u64; i < s.size(); i++) {
    h = (h ^ uint64_t(uint8_t(s[i]))) & 16777215u64;
    h = (h * 403u64) % 16777213u64;
  }
  return h;
}
""", [("int32_t", "vowels({})", ['"constant evaluation"']), ("int32_t", "vowels({})", ['""']),
      ("uint64_t", "hash({})", ['"gti"']), ("uint64_t", "hash({})", ['"a longer string with spaces"'])]),
    "floating": ("""
constexpr double horner(double x) {
  double coefficients[5] = {1.5f64, -2.25f64, 0.125f64, 3.0f64, -0.5f64};
  mut double r = 0.0f64;
  for (mut int32_t i = 0; i < 5; i++) { r = r * x + coefficients[i]; }
  return r;
}
constexpr float mixed(float x, int32_t k) { return x * float(k) / 3.0f32 - float(k) + x / 7.0f32; }
constexpr int64_t truncate(double x) { return int64_t(x * 1000.0f64); }
constexpr double divide(double a, double b) { return a / b; }
""", [("double", "horner({})", ["0.75f64"]), ("double", "horner({})", ["-3.5f64"]),
      ("float", "mixed({}, {})", ["2.5f32", 7]), ("int64_t", "truncate({})", ["-12.3456f64"]),
      ("double", "divide({}, {})", ["1.0f64", "-0.0f64"]), ("double", "divide({}, {})", ["123456789012345678901234567890.0f64", "0.0000000001f64"])]),
    "generic": ("""
concept scalar<T> = std::integral<T> || std::floating_point<T>;
T clamp_sum<scalar T>(T a, T b, T low, T high) {
  T s = a + b;
  if (s < low) { return low; }
  if (s > high) { return high; }
  return s;
}
constexpr T twice<scalar T>(T v) { return v + v; }
constexpr int64_t use_generic(int64_t x) { return twice(x) - int64_t(twice(int32_t(3))); }
""", [("int64_t", "use_generic({})", ["21i64"])]),
}


def shape_cells():
    cells = []
    for name, (declarations, calls) in SHAPES.items():
        checks = []
        for index, (type_name, pattern, args) in enumerate(calls):
            literals = [str(arg) for arg in args]
            ct = pattern.format(*literals)
            rt = pattern.format(*[f"opaque({arg})" for arg in literals])
            checks.append(f"  constexpr {type_name} v{index}_ct = {ct};\n"
                          f"  {type_name} v{index}_rt = {rt};\n"
                          f"  if (v{index}_ct != v{index}_rt) {{ std::println("
                          f"\"{name} {index} ct {{}} rt {{}}\", v{index}_ct, v{index}_rt); return 1; }}\n")
        source = (PRELUDE + declarations + "int main() {\n" + "".join(checks)
                  + "  std::println(\"ok\");\n  return 0;\n}\n")
        cells.append((f"shapes/{name}", source, {"expect_stdout": "ok\n"}))
    return cells


def control_cell():
    source = (PRELUDE + "constexpr int32_t f(int32_t a) { return a * 3; }\n"
              "int main() {\n  constexpr int32_t ct = f(4);\n"
              "  int32_t rt = f(opaque(4)) + 1;\n"
              "  if (ct != rt) { std::println(\"control ct {} rt {}\", ct, rt); return 1; }\n"
              "  std::println(\"ok\");\n  return 0;\n}\n")
    return [("control", source, {"expect_stdout": "ok\n"})]


def options(parser):
    parser.add_argument("--seed", type=int, default=13)
    parser.add_argument("--count", type=int, default=48,
                        help="random expressions per integer type")
    parser.add_argument("--family", nargs="*",
                        default=["arith", "small", "shapes"])
    parser.add_argument("--control", action="store_true",
                        help="run only the negative control (must fail)")
    parser.add_argument("--jobs", type=int, default=2)


def main():
    import concurrent.futures
    matrix = matrix_support.Matrix(__doc__.split("\n\n")[0],
                                   "constexpr-equivalence",
                                   default_levels="O0,O2", extra=options)
    args = matrix.args
    rng = random.Random(args.seed)
    cells = []
    if args.control:
        cells = control_cell()
    else:
        if "arith" in args.family:
            cells += arith_cells(rng, args.count, TYPES, True)
        if "small" in args.family:
            cells += arith_cells(rng, args.count // 2, SMALL, True)
        if "shapes" in args.family:
            cells += shape_cells()

    def one(cell):
        key, source, opts = cell
        path = matrix.out / (key.replace("/", "__") + ".gti")
        path.write_text(source, encoding="utf-8")
        return key, matrix.classify(path, key=key, **opts)

    failures = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for key, verdict in pool.map(one, cells):
            print(f"{key:40} {verdict}", flush=True)
            if matrix_support.hard(verdict) or verdict.startswith("WRONG-REJECTION"):
                failures.append(key)
    print(f"{len(cells)} cells at {','.join(matrix.levels)}: "
          f"{len(failures)} hard failures")
    sys.exit(matrix.finish(failures))


if __name__ == "__main__":
    main()
