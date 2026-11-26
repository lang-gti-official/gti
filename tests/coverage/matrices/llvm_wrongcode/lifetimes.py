"""Matrix: functions and classes with lifetime parameters, and invalid siblings.

Contract (docs/language/ownership-and-lifetimes.md "Lifetime parameters and
elision"): a lifetime parameter names a relationship, not a duration, and
regions are erased at the representation boundary, so a valid program
behaves exactly like its reference-free equivalent. A result region names
as suppliers exactly the parameters whose types mention it, and callers
keep every supplier borrowed while the result lives. A supplier that does
not outlive the result's region is `GTI-S2127`; returning a local, or a
parameter the result region does not name (as tests/ui/classes/
out_of_class_methods/invalid_borrow_contract_2.gti pins), is `GTI-S2017`, as is
using a borrow after its owner ended or mutating a supplier while a result
borrowed from it lives. A lifetime parameter is not a value (`GTI-S2128`).

Valid cells read and write through region-annotated references and check
the values against their literal inputs; each invalid sibling differs from
a valid cell by one change the rules forbid and must be rejected with the
documented code (an accepted invalid sibling is reported as WRONG).

Axes: shape (identity, a shared region over two inputs, a region naming one
of two inputs, a mutable region, a generic element region, a class storing
one or two regions, a method relaying a stored region, a nested relay
through another region function) x use (read after the call, write
through, keep across an inner scope, pass through a loop); each valid shape
has an invalid sibling.

Excluded shapes: region-annotated string views and spans (covered by
borrowed_elements); inference-only features from unmerged RFC drafts.
"""

PRELUDE = """import std.format;
import std.vector;
import std.utility;
int32_t&<a> identity<lifetime a>(int32_t&<a> value) { return value; }
int32_t&<a> choose<lifetime a>(int32_t&<a> left, int32_t&<a> right, bool first) {
  if (first) { return left; }
  return right;
}
int32_t&<a> pick<lifetime a>(int32_t&<a> first, int32_t& second) {
  if (second > 100) { return first; }
  return first;
}
mut int32_t&<a> bump<lifetime a>(mut int32_t&<a> value) {
  value += 1;
  return value;
}
T&<a> element<T, lifetime a>(std::vector<T>&<a> values, uint64_t index) requires std::vector_element<T> {
  return values[index];
}
int32_t&<a> relay<lifetime a>(int32_t&<a> left, int32_t&<a> right) {
  int32_t& local = choose(left, right, false);
  return identity(local);
}
struct Reading<lifetime data> {
  int32_t&<data> value;
  Reading(int32_t&<data> input) : value(input) {}
  int32_t&<data> get() { return this.value; }
};
struct Pair<lifetime a, lifetime b> {
  int32_t&<a> left;
  int32_t&<b> right;
  Pair(int32_t&<a> left, int32_t&<b> right) : left(left), right(right) {}
  int32_t&<a> first() { return this.left; }
  int32_t sum() { return this.left + this.right; }
};
class Table {
public:
  mut std::vector<int32_t> values;
  Table() : values(std::vector<int32_t>()) {}
};
class View<lifetime a> {
  Table&<a> table;
public:
  View(Table&<a> table) : table(table) {}
  int32_t&<a> get_or(uint64_t key, int32_t&<a> fallback) {
    if (key < this.table.values.size()) { return this.table.values[key]; }
    return fallback;
  }
};
"""

VALID = {
    "identity_read": """  int32_t x = 7;
  int32_t& r = identity(identity(x));
  if (r != 7) return 1;""",
    "choose_both": """  int32_t left = 3;
  int32_t right = 9;
  int32_t& a = choose(left, right, true);
  int32_t& b = choose(left, right, false);
  if (a * 10 + b != 39) return 1;""",
    "pick_one_supplier": """  int32_t kept = 4;
  mut int32_t other = 5;
  int32_t& r = pick(kept, other);
  other = 50;
  if (r != 4 || other != 50) return 1;""",
    "mutable_region": """  mut int32_t counter = 1;
  {
    mut int32_t& r = bump(counter);
    r += 10;
  }
  [[discard]] bump(counter);
  if (counter != 13) return 1;""",
    "generic_element": """  mut std::vector<int32_t> values{};
  values.push_back(4);
  values.push_back(6);
  int32_t& second = element(values, uint64_t(1));
  int32_t& first = element(values, uint64_t(0));
  if (first + second != 10) return 1;""",
    "nested_relay": """  int32_t left = 2;
  int32_t right = 8;
  int32_t& r = relay(left, right);
  if (r != 8) return 1;""",
    "class_one_region": """  int32_t source = 11;
  Reading reading{source};
  int32_t& kept = reading.get();
  Reading<_> again = Reading<_>(kept);
  if (again.get() + kept != 22) return 1;""",
    "class_two_regions_inner_scope": """  int32_t outer = 30;
  mut int32_t total = 0;
  {
    int32_t inner = 4;
    Pair pair = Pair(outer, inner);
    total = pair.sum();
    int32_t& first = pair.first();
    total += first;
  }
  if (total != 64) return 1;""",
    "method_stored_region": """  mut Table table{};
  table.values.push_back(5);
  table.values.push_back(7);
  int32_t fallback = 99;
  View view = View(table);
  int32_t& hit = view.get_or(uint64_t(1), fallback);
  int32_t& miss = view.get_or(uint64_t(5), fallback);
  if (hit != 7 || miss != 99) return 1;""",
    "loop_relay": """  mut std::vector<int32_t> values{};
  for (mut int32_t i = 1; i <= 4; i += 1) values.push_back(i * i);
  mut int32_t sum = 0;
  for (mut uint64_t i = uint64_t(0); i < values.size(); i += uint64_t(1)) {
    int32_t& item = identity(element(values, i));
    sum += item;
  }
  if (sum != 30) return 1;""",
}

# Invalid siblings: (body or declarations+body, expected diagnostic). Each
# mirrors the valid cell it is named after.
INVALID = {
    "choose_wrong_region": ("""int32_t&<a> wrong<lifetime a, lifetime b>(int32_t&<a> first, int32_t&<b> second) {
  return choose(first, second, true);
}
""", """  int32_t left = 3;
  int32_t right = 9;
  if (wrong(left, right) != 3) return 1;""", "S2127"),
    "pick_returns_unnamed": ("""int32_t&<a> pick_wrong<lifetime a>(int32_t&<a> first, int32_t& second) {
  return second;
}
""", """  int32_t kept = 4;
  int32_t other = 5;
  if (pick_wrong(kept, other) != 5) return 1;""", "S2017"),
    "identity_returns_local": ("""int32_t& escape(int32_t& value) {
  int32_t local = value;
  return local;
}
""", """  int32_t x = 7;
  if (escape(x) != 7) return 1;""", "S2017"),
    "choose_supplier_mutated": ("", """  mut int32_t left = 3;
  mut int32_t right = 9;
  int32_t& a = choose(left, right, true);
  right = 10;
  if (a != 3) return 1;""", "S2017"),
    "class_region_outlived": ("""Reading<a> keep<lifetime a>(int32_t&<a> value) { return Reading<a>(value); }
""", """  int32_t outer = 30;
  mut Reading<_> saved = keep(outer);
  {
    int32_t inner = 4;
    saved = keep(inner);
  }
  if (saved.get() != 4) return 1;""", "S2017"),
    "mutable_region_aliased": ("", """  mut int32_t counter = 1;
  mut int32_t& r = bump(counter);
  int32_t read = counter;
  r += 10;
  if (read != 2) return 1;""", "S2017"),
    "generic_element_after_growth": ("", """  mut std::vector<int32_t> values{};
  values.push_back(4);
  int32_t& first = element(values, uint64_t(0));
  values.push_back(6);
  if (first != 4) return 1;""", "S2017"),
    "lifetime_as_value": ("""int32_t bad<lifetime a>() { return a; }
""", """  if (bad() != 0) return 1;""", "S2128"),
}

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
    for name, body in VALID.items():
        yield name, PRELUDE + MAIN.replace("BODY", body)
    for name, (declarations, body, code) in INVALID.items():
        yield (f"invalid_{name}", PRELUDE + declarations + MAIN.replace("BODY", body),
               {"diagnostic": code})


# A deliberately broken variant the oracle must flag (the invalid sibling loses its forbidden write and is accepted):
# cell, original text, replacement. `--negative-controls` builds it.
NEGATIVE_CONTROL = ('invalid_choose_supplier_mutated',
                    '  right = 10;\n',
                    '')
