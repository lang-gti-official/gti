"""Matrix 9: virtual calls during construction and destruction.

The C++ backend gives no oracle here: during base destruction and constructor
rollback it dispatches to the most-derived override, a known defect
(docs/architecture/backend.md#lifecycle-dispatch). Each constructor and
destructor reports which override a virtual call on `this` selected, made in
its body or through one or two helpers that receive `this`. The program
compares the sequence with docs/language/execution.md 4.2.3: once a class's
base is initialized (or at entry, without a base) a call selects that class's
members, and while the destructor of C runs, through its own field cleanup,
a call resolves as if the dynamic type were C.
"""
import itertools
import re

PRELUDE = """#include <std/format>
#include <std/memory>
#include <std/utility>
#include <std/atomic>
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

# Codes: constructor of level k reports 10*k + selected override; destructor
# of level k reports 50 + 10*k + selected override; Mark fields report 90+k.
# Each shape: (source, levels, override visible while level k runs).
SHAPES = {
    "two_level": ("""class Root {
  Mark root_mark;
public:
  Root() : root_mark(Mark(91)) { CONSTRUCT(1) }
  ~Root() { DESTROY(1) }
  virtual int32_t code() { return 1; }
};
class Leaf : public Root {
  Mark leaf_mark;
public:
  Leaf() : Root(), leaf_mark(Mark(92)) { CONSTRUCT(2) }
  ~Leaf() { DESTROY(2) }
  int32_t code() override { return 2; }
};
""", [1, 2]),
    "three_level": ("""class Root {
  Mark root_mark;
public:
  Root() : root_mark(Mark(91)) { CONSTRUCT(1) }
  ~Root() { DESTROY(1) }
  virtual int32_t code() { return 1; }
};
class Mid : public Root {
  Mark mid_mark;
public:
  Mid() : Root(), mid_mark(Mark(92)) { CONSTRUCT(2) }
  ~Mid() { DESTROY(2) }
  int32_t code() override { return 2; }
};
class Leaf : public Mid {
  Mark leaf_mark;
public:
  Leaf() : Mid(), leaf_mark(Mark(93)) { CONSTRUCT(3) }
  ~Leaf() { DESTROY(3) }
  int32_t code() override { return 3; }
};
""", [1, 2, 3]),
    "inherited_middle": ("""class Root {
  Mark root_mark;
public:
  Root() : root_mark(Mark(91)) { CONSTRUCT(1) }
  ~Root() { DESTROY(1) }
  virtual int32_t code() { return 1; }
};
class Mid : public Root {
  Mark mid_mark;
public:
  Mid() : Root(), mid_mark(Mark(92)) { CONSTRUCT(2) }
  ~Mid() { DESTROY(2) }
};
class Leaf : public Mid {
  Mark leaf_mark;
public:
  Leaf() : Mid(), leaf_mark(Mark(93)) { CONSTRUCT(3) }
  ~Leaf() { DESTROY(3) }
  int32_t code() override { return 3; }
};
""", [1, 1, 3]),
}

# Each call form: (helpers, constructor report, destructor report).
CALLS = {
    "this_call": ("",
                  "note(10 * LEVEL + this.code());",
                  "note(50 + 10 * LEVEL + this.code());"),
    "helper": ("""void report_construct(Root& self, int32_t level) { note(10 * level + self.code()); }
void report_destroy(Root& self, int32_t level) { note(50 + 10 * level + self.code()); }
""", "report_construct(this, LEVEL);", "report_destroy(this, LEVEL);"),
    "nested_helper": ("""int32_t selected(Root& self) { return self.code(); }
void report_construct(Root& self, int32_t level) { note(10 * level + selected(self)); }
void report_destroy(Root& self, int32_t level) { note(50 + 10 * level + selected(self)); }
""", "report_construct(this, LEVEL);", "report_destroy(this, LEVEL);"),
}


def construct(levels):
    return [10 * (k + 1) + levels[k] for k in range(len(levels))]


def destroy(levels):
    sequence = []
    for k in reversed(range(len(levels))):
        sequence += [50 + 10 * (k + 1) + levels[k], 91 + k]
    return sequence


OWNERS = {
    "local": ("", """  {
    Leaf object = Leaf();
    note(5);
  }""", lambda c, d: c + [5] + d),
    "unique_base": ("", """  {
    std::unique_ptr<Root> owner = std::upcast_unique<Root, Leaf>(std::make_unique<Leaf>(Leaf()));
    note(5);
  }""", lambda c, d: c + [5] + d),
    "replace": ("", """  {
    mut Leaf object = Leaf();
    note(5);
    object = Leaf();
    note(6);
  }""", lambda c, d: c + [5] + c + d + [6] + d),
    "array": ("", """  {
    Leaf[2] objects = {Leaf(), Leaf()};
    note(5);
  }""", lambda c, d: c + c + [5] + d + d),
    "moved": ("", """  {
    mut Leaf first = Leaf();
    Leaf second = std::move(first);
    note(5);
  }""", lambda c, d: c + [5] + d),
}


def program(shape, call, owner):
    shape_source, levels = SHAPES[shape]
    helpers, body, want = OWNERS[owner]
    expected = want(construct(levels), destroy(levels))
    table = "".join(f"  if (index == {i}) return {code};\n"
                    for i, code in enumerate(expected))
    call_helpers, on_construct, on_destroy = CALLS[call]
    shape_source = re.sub(r"CONSTRUCT\((\d)\)",
                          lambda m: on_construct.replace("LEVEL", m.group(1)),
                          shape_source)
    shape_source = re.sub(r"DESTROY\((\d)\)",
                          lambda m: on_destroy.replace("LEVEL", m.group(1)),
                          shape_source)
    source = PRELUDE.replace("EXPECTED", table)
    # Namespace-scope declarations may refer to classes defined later.
    source += call_helpers + shape_source + helpers
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
    for shape, call, owner in itertools.product(SHAPES, CALLS, OWNERS):
        yield f"{shape}__{call}__{owner}", program(shape, call, owner)
