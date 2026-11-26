"""Matrix: owning payload enums in arrays, closures, callbacks and packs.

Contract (docs/language/static-semantics.md 3.8.2, "owning family"):
reference-free owning enums may be elements of fixed and nested arrays,
thread-local bindings and fields. A mutable constant-index element may be
moved out; replacement destroys the destination's previous payload; scope
exit destroys the remaining elements in reverse order; whole-array local
movement transfers the obligations. A lambda may own an enum through an
explicit move capture, and an unused or moved closure destroys it exactly
once, as does a stored callback. Concrete generic functions and parameter
packs transport each element with its own cleanup. A field replacement drops
the old alternative before the new one is published. Failure during argument
evaluation destroys only completed arguments (execution.md 4.2.3, 4.10).

Every Mark prints its code when destroyed, so the expected stdout is the
drop trace these rules give, then `ok`.

Axes: enum shape (a one-field alternative, a two-field alternative with a
passive sibling, a generic enum instance, an enum nested in an enum) x
transport (array move-out and replacement, nested arrays, whole-array move,
element swap, move capture, moved closure, stored callback, lambda by-value
parameter and result, pack-built holder, failing pack argument, field
replacement, vector erase and pop, optional replacement, generic identity,
thread-local array).

Excluded shapes: arrays of owning enums as closure captures and whole-array
by-value parameters or results (not admitted); namespace or static owning
arrays (not admitted); `std::array` of owning enums (no documented
admission); moving a value out of a capture (captures are immutable).
"""

PRELUDE = """import std.format;
import std.vector;
import std.optional;
import std.functional;
import std.utility;
import std.panic;
import std.atomic;
std::atomic<int32_t> unit{1};
int32_t one() { return unit.load(); }
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
"""

# shape: (declarations, TYPE, MAKE with C, EMPTY)
SHAPES = {
    "single": ("""enum E { none, owned(Mark m), pair(Mark a, int32_t n) };
int32_t code_of(E& e) {
  switch (e) {
  case E::owned(auto& m): return m.value();
  case E::pair(auto& a, n): return a.value() + n;
  case E::none: return 0;
  }
}
""", "E", "E::owned(Mark(C))", "E::none"),
    "pair": ("""enum E { none, owned(Mark m), pair(Mark a, int32_t n) };
int32_t code_of(E& e) {
  switch (e) {
  case E::owned(auto& m): return m.value();
  case E::pair(auto& a, n): return a.value() + n;
  case E::none: return 0;
  }
}
""", "E", "E::pair(Mark(C), 0)", "E::none"),
    "generic": ("""enum G<T> { none, owned(T m) };
int32_t code_of(G<Mark>& e) {
  switch (e) {
  case G<Mark>::owned(auto& m): return m.value();
  case G<Mark>::none: return 0;
  }
}
""", "G<Mark>", "G<Mark>::owned(Mark(C))", "G<Mark>::none"),
    "nested": ("""enum Inner { owned(Mark m) };
enum Outer { none, inner(Inner e) };
int32_t code_of(Outer& e) {
  switch (e) {
  case Outer::inner(auto& in):
    switch (in) {
    case Inner::owned(auto& m): return m.value();
    }
  case Outer::none: return 0;
  }
}
""", "Outer", "Outer::inner(Inner::owned(Mark(C)))", "Outer::none"),
}

HELPERS = """class Box {
public:
  mut TYPE slot;
  Box(TYPE slot) : slot(std::move(slot)) {}
};
class Holder {
public:
  TYPE first;
  int32_t n;
  TYPE second;
  Holder(TYPE first, int32_t n, TYPE second)
      : first(std::move(first)), n(n), second(std::move(second)) {}
};
T construct<T, Args...>(Args... args) { return T(args...); }
T identity<T>(T value) { return value; }
int32_t fail_value() {
  if (one() == 1) std::panic("pack failure");
  return 0;
}
"""

# transport: (body, trace, mode)
TRANSPORTS = {
    "array_move_out": ("""  {
    mut TYPE items[3] = {MAKE(1), EMPTY, MAKE(3)};
    auto taken = std::move(items[0]);
    items[1] = MAKE(2);
    items[2] = MAKE(4);
    note(50);
    if (code_of(taken) != 1 || code_of(items[1]) != 2 || code_of(items[2]) != 4) return 1;
  }""", [3, 50, 1, 4, 2]),
    "nested_array": ("""  {
    mut TYPE grid[2][2] = {{MAKE(1), MAKE(2)}, {MAKE(3), EMPTY}};
    grid[1][1] = MAKE(4);
    grid[0][0] = MAKE(5);
    note(50);
    if (code_of(grid[0][1]) + code_of(grid[1][1]) != 6) return 1;
  }""", [1, 50, 4, 3, 2, 5]),
    "array_whole_move": ("""  {
    mut TYPE first[2] = {MAKE(1), MAKE(2)};
    TYPE second[2] = std::move(first);
    note(50);
    if (code_of(second[1]) != 2) return 1;
  }""", [50, 2, 1]),
    "array_swap": ("""  {
    mut TYPE items[2] = {MAKE(1), MAKE(2)};
    TYPE held = std::move(items[0]);
    items[0] = std::move(items[1]);
    items[1] = std::move(held);
    note(50);
    if (code_of(items[0]) != 2 || code_of(items[1]) != 1) return 1;
  }""", [50, 1, 2]),
    "move_capture": ("""  {
    TYPE value = MAKE(5);
    auto read = [value = std::move(value)]() -> int32_t { return code_of(value); };
    if (read() != 5) return 1;
    note(50);
  }""", [50, 5]),
    "moved_closure": ("""  {
    TYPE value = MAKE(5);
    auto read = [value = std::move(value)]() -> int32_t { return code_of(value); };
    {
      auto moved = std::move(read);
      if (moved() != 5) return 1;
      note(50);
    }
    note(51);
  }""", [50, 5, 51]),
    "stored_callback": ("""  {
    TYPE value = MAKE(6);
    mut std::vector<std::move_only_function<int32_t()>> callbacks{};
    callbacks.push_back(std::move_only_function<int32_t()>(
        [value = std::move(value)]() -> int32_t { return code_of(value); }));
    if (callbacks[0]() != 6) return 1;
    note(50);
  }""", [50, 6]),
    "lambda_by_value": ("""  {
    auto relay = [](TYPE value) -> TYPE { note(40); return value; };
    TYPE out = relay(MAKE(7));
    note(50);
    if (code_of(out) != 7) return 1;
  }""", [40, 50, 7]),
    "pack_holder": ("""  {
    auto holder = construct<Holder>(MAKE(1), 7, MAKE(2));
    note(50);
    if (code_of(holder.first) + holder.n + code_of(holder.second) != 10) return 1;
  }""", [50, 2, 1]),
    "pack_failure": ("""  {
    auto holder = construct<Holder>(MAKE(1), fail_value(), MAKE(2));
    note(50);
  }""", [1], "failure"),
    "field_replace": ("""  {
    mut Box box = Box(MAKE(1));
    box.slot = MAKE(2);
    note(50);
    box.slot = EMPTY;
    note(51);
    box.slot = MAKE(3);
  }""", [1, 50, 2, 51, 3]),
    "vector_erase_pop": ("""  {
    mut std::vector<TYPE> items{};
    items.push_back(MAKE(1));
    items.push_back(MAKE(2));
    items.push_back(MAKE(3));
    items.erase(std::size_t(1));
    items.pop_back();
    note(50);
    if (items.size() != std::size_t(1) || code_of(items[0]) != 1) return 1;
  }""", [2, 3, 50, 1]),
    "optional_replace": ("""  {
    mut std::optional<TYPE> slot = std::optional<TYPE>(MAKE(3));
    slot = std::optional<TYPE>(MAKE(4));
    note(50);
    slot = std::optional<TYPE>();
    note(51);
  }""", [3, 50, 4, 51]),
    "generic_identity": ("""  {
    TYPE out = identity(identity(MAKE(4)));
    note(50);
    if (code_of(out) != 4) return 1;
  }""", [50, 4]),
}

TLS = """thread_local mut TYPE slots[2] = {MAKE(1), MAKE(2)};
int main() {
  slots[0] = MAKE(3);
  if (code_of(slots[0]) + code_of(slots[1]) != 5) return 1;
  std::println("ok");
  return 0;
}
"""

MAIN = """int32_t run() {
BODY
  note(90);
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


def instantiate(text, shape):
    _, type_, make, empty = SHAPES[shape]
    out = text.replace("TYPE", type_).replace("EMPTY", empty)
    while "MAKE(" in out:
        start = out.index("MAKE(")
        end = out.index(")", start)
        out = out[:start] + make.replace("C", out[start + 5:end]) + out[end + 1:]
    return out


def cells():
    for shape, (declarations, _, _, _) in SHAPES.items():
        for transport, cell in TRANSPORTS.items():
            body, trace = cell[0], cell[1]
            mode = cell[2] if len(cell) > 2 else None
            text = PRELUDE + declarations + instantiate(
                HELPERS + MAIN.replace("BODY", body), shape)
            lines = "".join(f"{code}\n" for code in trace)
            if mode == "failure":
                expect = {"stdout": lines, "returncode": 70,
                          "stderr": 'runtime error[GTI-R0017]: panic: "pack failure"'}
            else:
                expect = {"stdout": lines + "90\nok\n"}
            yield f"{shape}_{transport}", text, expect
        # The initial thread drains its thread-local array after main returns,
        # in reverse element order; the replaced payload drops at assignment.
        yield (f"{shape}_thread_local_array",
               PRELUDE + declarations + instantiate(TLS, shape),
               {"stdout": "1\nok\n2\n3\n"})


# A deliberately broken variant the oracle must flag (a second replacement drops the first new payload):
# cell, original text, replacement. `--negative-controls` builds it.
NEGATIVE_CONTROL = ('single_array_move_out',
                    'items[2] = E::owned(Mark(4));',
                    'items[2] = E::owned(Mark(4));\n    items[2] = E::owned(Mark(4));')
