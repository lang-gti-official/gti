"""Matrix: payload bindings in switch, `if case`, init statements and range-for.

Contract (docs/language/execution.md 4.5 "Statements", static-semantics.md
3.8.2 and 3.10.1, ranges.md "Structured bindings"): a payload switch or
`if case` evaluates its subject once. A named place is observed: by-value
bindings copy, `auto&` borrows and `mut auto&` writes through to the
payload. A consumed subject (a prvalue or `std::move(place)`) is retired at
the selected arm's entry: a binding arm moves the fields into the bindings,
which drop at the arm's end newest first (or on `break`, `continue`,
`return` or failure); a tag-only, `default` or mismatching branch drops the
enum at its entry. A fixed-array element subject checks its index once,
before any arm runs. An owning init statement is destroyed once, after the
taken branch, on every exit. Structured range bindings project each element
for one iteration.

Every Mark prints its code when destroyed; each cell's expected stdout is
the trace those rules give, then `ok` (or status 70 and the report for the
failure cells).

Axes: construct (switch, `if case`, switch and `if` init statements, range
for) x subject (prvalue, moved place, named place by copy, shared and
mutable reference bindings, array element) x exit (fallthrough, break,
continue, return, failure). The cells are the admitted combinations, each
hand-checked against the rules above.

Excluded shapes: `mut auto [a, b]` (not supported), map decomposition
(no structured binding over map entries is documented), partial moves out of
a structured binding (not supported), the drop order of two payload fields
of a tag-only consumed enum (not specified separately from field order, so
single-field alternatives are used there).
"""

PRELUDE = """import std.format;
import std.vector;
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
enum Env { empty, one(Mark m), two(Mark a, Mark b) };
enum Shared { none, item(int32_t c, Mark tag) };
Env make_one(int32_t code) { return Env::one(Mark(code)); }
Env make_two(int32_t a, int32_t b) { return Env::two(Mark(a), Mark(b)); }
Env make_empty() { return Env::empty; }
"""

# name: (body of run(), trace, expectation override)
CELLS = {
    "switch_prvalue_bindings": ("""  switch (make_two(1, 2)) {
  case Env::two(a, b):
    note(a.value() + b.value() + 50);
    break;
  case Env::one(m):
    return 1;
  case Env::empty:
    return 2;
  }
  note(90);""", [53, 2, 1, 90]),
    "switch_prvalue_tag_only": ("""  switch (make_one(3)) {
  case Env::one:
    note(50);
    break;
  case Env::two:
    return 1;
  case Env::empty:
    return 2;
  }
  note(90);""", [3, 50, 90]),
    "switch_prvalue_default": ("""  switch (make_one(3)) {
  case Env::empty:
    return 1;
  default:
    note(50);
    break;
  }
  note(90);""", [3, 50, 90]),
    "switch_moved_place": ("""  {
    mut Env held = make_one(4);
    switch (std::move(held)) {
    case Env::one(m):
      note(50);
      break;
    default:
      return 1;
    }
    note(51);
  }
  note(90);""", [50, 4, 51, 90]),
    "switch_moved_place_tag_only": ("""  {
    mut Env held = make_one(4);
    switch (std::move(held)) {
    case Env::empty:
      return 1;
    default:
      note(50);
      break;
    }
    note(51);
  }
  note(90);""", [4, 50, 51, 90]),
    "switch_named_shared_ref": ("""  {
    Env held = make_two(5, 6);
    switch (held) {
    case Env::two(auto& a, auto& b):
      note(a.value() * 10 + b.value());
      break;
    default:
      return 1;
    }
    note(51);
  }
  note(90);""", [56, 51, "two_drop", 90]),
    "switch_named_mut_ref": ("""  {
    mut Env held = make_one(4);
    switch (held) {
    case Env::one(mut auto& m):
      m = Mark(7);
      note(50);
      break;
    default:
      return 1;
    }
    note(51);
    switch (held) {
    case Env::one(auto& m):
      if (m.value() != 7) return 2;
      break;
    default:
      return 3;
    }
  }
  note(90);""", [4, 50, 51, 7, 90]),
    "switch_named_copy": ("""  {
    Shared held = Shared::item(8, Mark(4));
    switch (held) {
    case Shared::item(c, auto& tag):
      note(c + tag.value() * 10);
      break;
    case Shared::none:
      return 1;
    }
    note(51);
  }
  note(90);""", [48, 51, 4, 90]),
    "switch_return_binding": ("""  {
    Mark kept = take(make_one(9));
    note(50);
  }
  note(90);""", [50, 9, 90]),
    "switch_failure_in_arm": ("""  switch (make_two(1, 2)) {
  case Env::two(a, b):
    note(50);
    if (one() == 1) std::panic("arm failure");
    break;
  default:
    return 1;
  }
  note(90);""", [50, 2, 1], "failure"),
    "if_case_prvalue_match": ("""  if (case Env::one(m) = make_one(5)) {
    note(m.value() + 50);
  } else {
    return 1;
  }
  note(90);""", [55, 5, 90]),
    "if_case_named_mismatch": ("""  {
    Env held = make_one(5);
    if (case Env::two(auto& a, auto& b) = held) {
      return 1;
    } else if (case Env::one(auto& m) = held) {
      note(m.value() + 60);
    }
    note(51);
  }
  note(90);""", [65, 51, 5, 90]),
    "if_case_loop_exits": ("""  for (mut int32_t i = 1; i <= 4; i += 1) {
    if (case Env::one(m) = make_one(10 + i)) {
      if (i == 2) continue;
      if (i == 4) break;
      note(m.value() + 20);
    }
  }
  note(90);""", [31, 11, 12, 33, 13, 14, 90]),
    "if_case_return": ("""  if (first_large(3) != 13) return 1;
  note(90);""", [11, 12, 50, 13, 90]),
    "switch_init_statement": ("""  {
    switch (Env held = make_one(6); held) {
    case Env::one(auto& m):
      note(m.value() + 50);
      break;
    default:
      return 1;
    }
    note(51);
  }
  note(90);""", [56, 6, 51, 90]),
    "if_init_statement_exits": ("""  for (mut int32_t i = 1; i <= 3; i += 1) {
    if (Mark m = Mark(20 + i); i == 2) {
      continue;
    } else {
      note(m.value() + 10);
    }
    note(40 + i);
  }
  note(90);""", [31, 21, 41, 22, 33, 23, 43, 90]),
    "for_init_statement": ("""  for (std::vector<Mark> marks = one_mark(7); auto& mark : marks) {
    note(mark.value() + 50);
  }
  note(90);""", [57, 7, 90]),
    "array_element_subject": ("""  {
    mut Env items[3] = {make_one(1), make_empty(), make_one(3)};
    mut int32_t total = 0;
    for (mut int32_t i = 0; i < 3; i += 1) {
      switch (items[i]) {
      case Env::one(auto& m):
        total += m.value();
        break;
      default:
        total += 10;
        break;
      }
    }
    if (total != 14) return 1;
    note(50);
  }
  note(90);""", [50, 3, 1, 90]),
    "array_element_out_of_range": ("""  mut Env items[2] = {make_one(1), make_one(2)};
  note(50);
  switch (items[one() + 1]) {
  case Env::one(auto& m):
    note(60);
    break;
  default:
    note(61);
    break;
  }
  note(70);""", [50, 2, 1], "index"),
    "range_structured_bindings": ("""  mut std::vector<Pair> entries{};
  entries.push_back(Pair(1, 10));
  entries.push_back(Pair(2, 20));
  mut int32_t sum = 0;
  for (auto [key, value] : entries) { sum += key * value; }
  for (auto& [key, value] : entries) { sum += key + value; }
  for (mut auto& [key, value] : entries) { key += 1; value += key; }
  for (auto& entry : entries) { sum += entry.key * 1000 + entry.value; }
  if (sum != 50 + 33 + 2012 + 3023) return 1;
  note(90);""", [90]),
    # `?` producing the subject of `if case`, a switch init statement and a
    # consuming switch in a loop; the error edge cleans only live owners.
    "propagated_subjects": ("""  auto a = probe(3);
  note(90);
  auto b = probe(0);
  note(91);
  auto c = probe(-2);
  note(92);
  if (!a || a.value() != 3 || !b || b.value() != 0 || c || c.error() != -2) return 1;
  mut std::vector<int32_t> raws{};
  raws.push_back(2);
  raws.push_back(0);
  raws.push_back(-7);
  raws.push_back(4);
  auto s = sum_all(raws);
  if (s || s.error() != -7) return 2;""", [13, 3, 1, 90, 25, 5, 1, 91, 1, 92, 2, 42, 40, 33]),
    "structured_binding_hidden_owner": ("""  {
    auto [first, second] = MarkPair(Mark(1), Mark(2));
    note(first.value() + second.value() + 50);
  }
  note(90);""", [53, "pair_drop", 90]),
}

HELPERS = """Mark take(Env env) {
  switch (std::move(env)) {
  case Env::one(m):
    return m;
  default:
    return Mark(0);
  }
}
int32_t first_large(int32_t limit) {
  for (mut int32_t i = 1; i <= limit; i += 1) {
    if (case Env::one(m) = make_one(10 + i)) {
      if (i == limit) { note(50); return m.value(); }
    }
  }
  return 0;
}
std::vector<Mark> one_mark(int32_t code) {
  mut std::vector<Mark> marks{};
  marks.push_back(Mark(code));
  return marks;
}
expected<Env, int32_t> fetch(int32_t raw) {
  if (raw < 0) return unexpected(raw);
  if (raw == 0) return Env::empty;
  return Env::one(Mark(raw));
}
expected<int32_t, int32_t> probe(int32_t raw) {
  Mark guard = Mark(1);
  if (case Env::one(m) = ?fetch(raw)) {
    note(m.value() + 10);
    return m.value();
  }
  switch (Env e = ?fetch(raw + 5); e) {
  case Env::one(auto& m):
    note(m.value() + 20);
    break;
  default:
    break;
  }
  return 0;
}
expected<int32_t, int32_t> sum_all(std::vector<int32_t>& raws) {
  mut int32_t total = 0;
  for (int32_t raw : raws) {
    Mark step = Mark(40 + raw);
    switch (?fetch(raw)) {
    case Env::one(m):
      total += m.value();
      break;
    case Env::empty:
      total += 100;
      break;
    default:
      return unexpected(-99);
    }
  }
  return total;
}
struct Pair {
  mut int32_t key;
  mut int32_t value;
  Pair(int32_t key, int32_t value) : key(key), value(value) {}
};
struct MarkPair {
  Mark first;
  Mark second;
  MarkPair(Mark first, Mark second) : first(std::move(first)), second(std::move(second)) {}
};
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

# Two-field payloads and two-field structs drop in reverse field order
# (ownership-and-lifetimes.md: fields end in reverse declaration order).
SYMBOLS = {"two_drop": [6, 5], "pair_drop": [2, 1]}


def expectation(trace, mode):
    lines = []
    for event in trace:
        lines.extend(SYMBOLS.get(event, [event]) if isinstance(event, str) else [event])
    stdout = "".join(f"{line}\n" for line in lines)
    if mode == "failure":
        return {"stdout": stdout, "returncode": 70,
                "stderr": 'runtime error[GTI-R0017]: panic: "arm failure"'}
    if mode == "index":
        return {"stdout": stdout, "returncode": 70,
                "stderr": "runtime error[GTI-R0007]: index_out_of_bounds in fixed_array"}
    return {"stdout": stdout + "ok\n"}


def cells():
    for name, cell in CELLS.items():
        body, trace = cell[0], cell[1]
        mode = cell[2] if len(cell) > 2 else None
        yield (name, PRELUDE + HELPERS + MAIN.replace("BODY", body),
               expectation(trace, mode))


# A deliberately broken variant the oracle must flag (an extra arm-scoped owner drops before the bindings):
# cell, original text, replacement. `--negative-controls` builds it.
NEGATIVE_CONTROL = ('switch_prvalue_bindings',
                    '    note(a.value() + b.value() + 50);\n    break;',
                    '    note(a.value() + b.value() + 50);\n    Mark extra = Mark(7);\n    break;')
