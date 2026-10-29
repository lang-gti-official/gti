"""Matrix 8: destruction order, class shape x end of lifetime.

Counting destructions cannot see an order defect, so each Mark and each
destructor body reports a code, and the program compares the sequence with
the order GTI specifies (docs/language/execution.md 4.2.3): a destructor body
runs first, then fields in reverse declaration order, then the state-bearing
base; locals, parameters, captures and array elements end in reverse
successful-initialization order; a move reparents the obligation, so a
moved-from object reports nothing.
"""
import itertools

PRELUDE = """import std.format;
import std.memory;
import std.utility;
import std.atomic;
std::atomic<int32_t> position{0};
std::atomic<int32_t> first_bad{-1};
int32_t expected_at(int32_t index) {
EXPECTED  return -1;
}
void note(int32_t code) {
  int32_t at = position.fetch_add(1);
  if (first_bad.load() < 0 && expected_at(at) != code) first_bad.store(at);
}
class Mark {
  int32_t code;
public:
  Mark(int32_t code) : code(code) {}
  Mark(Mark& other) = delete;
  Mark(Mark&& other) = default;
  ~Mark() { note(this.code); }
};
"""

# Each shape: (source, destruction sequence of Obj(t)).
SHAPES = {
    "plain": ("""class Obj {
  Mark first;
  Mark second;
public:
  Obj(int32_t t) : first(Mark(t * 10 + 1)), second(Mark(t * 10 + 2)) {}
  ~Obj() { note(this.tag()); }
  int32_t tag() { return 0; }
};
""", lambda t: [0, t * 10 + 2, t * 10 + 1]),
    "derived": ("""class Base {
  Mark low;
public:
  Base(int32_t t) : low(Mark(t * 10 + 1)) {}
  ~Base() { note(this.base_code); }
  int32_t base_code = 0;
};
class Obj : public Base {
  Mark middle;
  Mark high;
public:
  Obj(int32_t t) : Base(t), middle(Mark(t * 10 + 2)), high(Mark(t * 10 + 3)) {}
  ~Obj() { note(this.base_code + 1); }
};
""", lambda t: [1, t * 10 + 3, t * 10 + 2, 0, t * 10 + 1]),
    "three_level": ("""class Root {
  Mark r;
public:
  Root(int32_t t) : r(Mark(t * 10 + 1)) {}
  ~Root() { note(7); }
};
class Mid : public Root {
  Mark m;
public:
  Mid(int32_t t) : Root(t), m(Mark(t * 10 + 2)) {}
  ~Mid() { note(8); }
};
class Obj : public Mid {
  Mark l;
public:
  Obj(int32_t t) : Mid(t), l(Mark(t * 10 + 3)) {}
  ~Obj() { note(9); }
};
""", lambda t: [9, t * 10 + 3, 8, t * 10 + 2, 7, t * 10 + 1]),
    "interface": ("""interface Shape {
  int32_t sides();
};
class Obj : public Shape {
  Mark first;
  Mark second;
public:
  Obj(int32_t t) : first(Mark(t * 10 + 1)), second(Mark(t * 10 + 2)) {}
  ~Obj() { note(6); }
  int32_t sides() override { return 4; }
};
""", lambda t: [6, t * 10 + 2, t * 10 + 1]),
}

# Each operation: (helpers, body, expected sequence given seq(t)).
OPS = {
    "scope": ("", """  {
    Obj o = Obj(1);
    note(95);
  }""", lambda s: [95] + s(1)),
    "two_locals": ("", """  {
    Obj a = Obj(1);
    Obj b = Obj(2);
    note(95);
  }""", lambda s: [95] + s(2) + s(1)),
    "replace": ("", """  {
    mut Obj a = Obj(1);
    a = Obj(2);
    note(95);
  }""", lambda s: s(1) + [95] + s(2)),
    "moved_local": ("", """  {
    mut Obj a = Obj(1);
    {
      Obj b = std::move(a);
      note(94);
    }
    note(95);
  }""", lambda s: [94] + s(1) + [95]),
    "unique": ("", """  {
    std::unique_ptr<Obj> owner = std::make_unique<Obj>(Obj(1));
    std::unique_ptr<Obj> moved = std::move(owner);
    note(95);
  }""", lambda s: [95] + s(1)),
    "byvalue": ("void consume(Obj o) { note(93); }\n", """  consume(Obj(1));
  note(95);""", lambda s: [93] + s(1) + [95]),
    "parameters": ("void consume2(Obj a, Obj b) { note(93); }\n", """  consume2(Obj(1), Obj(2));
  note(95);""", lambda s: [93] + s(2) + s(1) + [95]),
    "moved_argument": ("void consume(Obj o) { note(93); }\n", """  {
    mut Obj a = Obj(1);
    consume(std::move(a));
    note(95);
  }""", lambda s: [93] + s(1) + [95]),
    "closure": ("", """  {
    mut Obj a = Obj(1);
    mut Obj b = Obj(2);
    auto f = [first = std::move(a), second = std::move(b)]() -> int32_t {
      note(93);
      return 0;
    };
    int32_t ignored = f();
    note(95);
  }""", lambda s: [93, 95] + s(2) + s(1)),
    "returned": ("""Obj build(int32_t t) {
  Obj kept = Obj(t);
  Obj dropped = Obj(t + 1);
  return kept;
}
""", """  {
    Obj r = build(1);
    note(95);
  }""", lambda s: s(2) + [95] + s(1)),
    "early_return": ("""int32_t leave(bool early) {
  Obj outer = Obj(1);
  if (early) {
    Obj inner = Obj(2);
    return 3;
  }
  note(92);
  return 4;
}
""", """  int32_t first = leave(true);
  note(95);""", lambda s: s(2) + s(1) + [95]),
    "loop_break": ("", """  for (mut int32_t i = 1; i < 4; i += 1) {
    Obj o = Obj(i);
    if (i == 2) break;
    note(95);
  }""", lambda s: [95] + s(1) + s(2)),
    "array": ("", """  {
    Obj[2] items = {Obj(1), Obj(2)};
    note(95);
  }""", lambda s: [95] + s(2) + s(1)),
    "outer_fields": ("""class Outer {
  Obj first;
  Obj second;
public:
  Outer() : first(Obj(1)), second(Obj(2)) {}
  ~Outer() { note(91); }
};
""", """  {
    Outer o = Outer();
    note(95);
  }""", lambda s: [95, 91] + s(2) + s(1)),
    "payload": ("enum Slot { held(Obj value), none };\n", """  {
    Slot s = Slot::held(Obj(1));
    note(95);
  }""", lambda s: [95] + s(1)),
}


def program(shape, op):
    shape_source, seq = SHAPES[shape]
    helpers, body, want = OPS[op]
    expected = want(seq)
    table = "".join(f"  if (index == {i}) return {code};\n"
                    for i, code in enumerate(expected))
    source = PRELUDE.replace("EXPECTED", table) + shape_source + helpers
    source += f"""int main() {{
{body}
  if (first_bad.load() >= 0 || position.load() != {len(expected)}) {{
    std::println("order {{}} {{}}", first_bad.load(), position.load());
    return 2;
  }}
  std::println("ok");
  return 0;
}}
"""
    return source


def cells():
    for shape, op in itertools.product(SHAPES, OPS):
        yield f"{shape}__{op}", program(shape, op)
