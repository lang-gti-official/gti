"""Matrix: borrowed container elements, owner-tied cursors and element yields.

Contract (docs/language/ranges.md "Element references and their container"
and "Cursor-region yields", ownership-and-lifetimes.md "Standard-Library
Storage"): a shared cursor's `operator*` returns a reference in the region
of the container it stores, so the element stays valid after the cursor
advances, outlives a temporary cursor, and can be returned through a
reference parameter. While it is live the container cannot be mutated,
moved or destroyed. A user-defined `cursor<lifetime owner>` gets the same
behaviour by naming the owner region in its dereference result.

Each cell fills a container with the literal sequence 4, 1, 3, 2 (or text
whose length and first letter encode those numbers) and checks the values
read through borrowed elements against that sequence.

Axes: container (vector of integers, vector of strings, vector of owning
Marks, deque, list, span over a vector, a user owner-tied cursor over
integers and over string views) x access (element kept across an advance,
reverse begin, max_element, a reference returned through a parameter,
operator[], front and back, a range-for reference sum, a temporary cursor,
find).

Excluded shapes: `paired_span` and `std::array` cursors (cursor-tied by
design); exclusive cursors (`begin_mut`, results stay cursor-tied);
operator[] on list and on the user cursors (not provided).
"""

PRELUDE = """import std.format;
import std.vector;
import std.deque;
import std.list;
import std.span;
import std.string;
import std.string_view;
import std.algorithm;
import std.iterator;
import std.utility;
"""

TEXT = {4: "d" * 40, 1: "a" * 10, 3: "c" * 30, 2: "b" * 20}

# container: (declarations, fill statements declaring `c`, element type,
# value expression VAL(x) for an element x, supported accesses)
ALL = ("first_after_advance", "rbegin", "max_element", "head_param", "index",
       "front_back", "range_sum", "temporary_cursor", "find")


def push(kind, values):
    return "".join(f"  c.push_back({kind(v)});\n" for v in values)


CONTAINERS = {
    "vector_int": ("", "  mut std::vector<int32_t> c{};\n" + push(lambda v: v, [4, 1, 3, 2]),
                   "std::vector<int32_t>", "int32_t", "X", ALL),
    "vector_string": ("", "  mut std::vector<std::string> c{};\n"
                      + push(lambda v: f'std::string("{TEXT[v]}")', [4, 1, 3, 2]),
                      "std::vector<std::string>", "std::string", "int32_t(X.size()) / 10",
                      tuple(a for a in ALL if a != "find")),
    "vector_owner": ("""class Mark {
  int32_t code;
public:
  Mark(int32_t code) : code(code) {}
  Mark(Mark& other) = delete;
  Mark(Mark&& other) = default;
  int32_t value() { return this.code; }
  bool operator<(Mark& other) { return this.code < other.code; }
};
""", "  mut std::vector<Mark> c{};\n" + push(lambda v: f"Mark({v})", [4, 1, 3, 2]),
                     "std::vector<Mark>", "Mark", "X.value()",
                     tuple(a for a in ALL if a != "find")),
    "deque": ("", "  mut std::deque<int32_t> c{};\n" + push(lambda v: v, [4, 1, 3, 2]),
              "std::deque<int32_t>", "int32_t", "X", ALL),
    "list": ("", "  mut std::list<int32_t> c{};\n" + push(lambda v: v, [4, 1, 3, 2]),
             "std::list<int32_t>", "int32_t", "X",
             tuple(a for a in ALL if a != "index")),
    "span": ("", "  mut std::vector<int32_t> storage{};\n"
             + push(lambda v: v, [4, 1, 3, 2]).replace("c.push_back", "storage.push_back")
             + "  std::span<int32_t> c = storage.as_span();\n",
             "std::span<int32_t>", "int32_t", "X", ALL),
    "user_cursor": ("""struct Numbers {
  mut int32_t values[4];
  Numbers() : values({4, 1, 3, 2}) {}
};
class sentinel { public: uint64_t at; sentinel(uint64_t at) : at(at) {} };
class cursor<lifetime owner> {
  Numbers&<owner> data;
  mut uint64_t position;
public:
  cursor(Numbers&<owner> data, uint64_t position) : data(data), position(position) {}
  cursor(cursor<owner>& other) = default;
  int32_t&<owner> operator*() { return this.data.values[this.position]; }
  void operator++() mut { this.position = this.position + uint64_t(1); }
  bool operator!=(sentinel end) { return this.position != end.at; }
  bool operator!=(cursor<owner>& other) { return this.position != other.position; }
  bool operator==(cursor<owner>& other) { return this.position == other.position; }
};
class Bag {
public:
  Numbers numbers;
  Bag() : numbers(Numbers()) {}
  cursor<this> begin() { return cursor(this.numbers, uint64_t(0)); }
  sentinel end() { return sentinel(uint64_t(4)); }
};
""", "  Bag c{};\n", "Bag", "int32_t", "X",
                    ("first_after_advance", "head_param", "range_sum", "temporary_cursor")),
    "user_view_cursor": ("""struct Words {
  mut std::string_view values[4];
  Words() : values({"WORD4", "W1", "WRD3", "WW2"}) {}
};
class sentinel { public: uint64_t at; sentinel(uint64_t at) : at(at) {} };
class cursor<lifetime owner> {
  Words&<owner> data;
  mut uint64_t position;
public:
  cursor(Words&<owner> data, uint64_t position) : data(data), position(position) {}
  cursor(cursor<owner>& other) = default;
  std::string_view&<owner> operator*() { return this.data.values[this.position]; }
  void operator++() mut { this.position = this.position + uint64_t(1); }
  bool operator!=(sentinel end) { return this.position != end.at; }
  bool operator!=(cursor<owner>& other) { return this.position != other.position; }
  bool operator==(cursor<owner>& other) { return this.position == other.position; }
};
class Bag {
public:
  Words words;
  Bag() : words(Words()) {}
  cursor<this> begin() { return cursor(this.words, uint64_t(0)); }
  sentinel end() { return sentinel(uint64_t(4)); }
};
""", "  Bag c{};\n", "Bag", "std::string_view", "int32_t(X.size()) - 1",
                         ("first_after_advance", "head_param", "range_sum", "temporary_cursor")),
}

# access: (helper declarations using C and E, body using VAL; returns a
# status)
ACCESSES = {
    "first_after_advance": ("", """  mut auto it = c.begin();
  auto& first = *it;
  ++it;
  auto& second = *it;
  ++it;
  if (VAL(first) != 4 || VAL(second) != 1 || VAL(*it) != 3) return 1;"""),
    "rbegin": ("", """  auto& last = *c.rbegin();
  if (VAL(last) != 2) return 1;"""),
    "max_element": ("", """  auto& largest = *std::max_element(c.begin(), c.end());
  auto& smallest = *std::min_element(c.begin(), c.end());
  if (VAL(largest) != 4 || VAL(smallest) != 1) return 1;"""),
    "head_param": ("E& head(C& values) { return *values.begin(); }\n", """  auto& first = head(c);
  auto& again = head(c);
  if (VAL(first) + VAL(again) != 8) return 1;"""),
    "index": ("", """  auto& third = c[2];
  auto& first = c[0];
  if (VAL(third) != 3 || VAL(first) != 4) return 1;"""),
    "front_back": ("", """  auto& front = c.front();
  auto& back = c.back();
  if (VAL(front) * 10 + VAL(back) != 42) return 1;"""),
    "range_sum": ("", """  mut int32_t sum = 0;
  mut int32_t weighted = 0;
  mut int32_t at = 1;
  for (auto& x : c) {
    sum += VAL(x);
    weighted += at * VAL(x);
    at += 1;
  }
  if (sum != 10 || weighted != 4 + 2 + 9 + 8) return 1;"""),
    "temporary_cursor": ("", """  auto& again = *c.begin();
  mut auto it = c.begin();
  ++it;
  if (VAL(again) != 4 || VAL(*it) != 1) return 1;"""),
    "find": ("", """  mut auto found = std::find(c.begin(), c.end(), 3);
  auto& three = *found;
  ++found;
  if (VAL(three) != 3 || VAL(*found) != 2) return 1;"""),
}

MAIN = """int32_t run() {
FILL
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


def substitute_val(text, value):
    out = text
    while "VAL(" in out:
        start = out.index("VAL(")
        depth, end = 0, start + 3
        while True:
            if out[end] == "(":
                depth += 1
            elif out[end] == ")":
                depth -= 1
                if depth == 0:
                    break
            end += 1
        inner = out[start + 4:end]
        out = out[:start] + "(" + value.replace("X", "(" + inner + ")") + ")" + out[end + 1:]
    return out


def program(container, access):
    declarations, fill, ctype, etype, value, _ = CONTAINERS[container]
    helpers, body = ACCESSES[access]
    helpers = helpers.replace("C&", ctype + "&").replace("E&", etype + "&")
    main = MAIN.replace("FILL", fill.rstrip("\n")).replace("BODY", substitute_val(body, value))
    return PRELUDE + declarations + helpers + main


# Element references through a region function, a stored region, a lambda
# with a reference parameter, a map row and structured map-entry bindings.
SCENARIOS = {
    "relayed_elements": """import std.map;
int32_t&<a> largest<lifetime a>(std::vector<int32_t>&<a> values) { return *std::max_element(values.begin(), values.end()); }
struct Slot<lifetime a> {
  int32_t&<a> value;
  Slot(int32_t&<a> value) : value(value) {}
};
int main() {
  mut std::vector<int32_t> values{};
  values.push_back(3); values.push_back(9); values.push_back(4);
  auto pick = [](std::vector<int32_t>& v) -> int32_t& { return v[1]; };
  int32_t& via_lambda = pick(values);
  int32_t& big = largest(values);
  Slot<_> slot = Slot<_>(big);
  mut std::map<std::string, std::vector<int32_t>> table{};
  table[std::string(\"k\")] = std::vector<int32_t>();
  table[std::string(\"k\")].push_back(7);
  std::vector<int32_t>& row = table.at(std::string(\"k\"));
  int32_t& first = row[0];
  mut int32_t sum = 0;
  for (auto& [key, list] : table) { sum += int32_t(key.size()) + list[0]; }
  if (via_lambda != 9 || slot.value != 9 || first != 7 || sum != 8) { std::println(\"{} {} {} {}\", via_lambda, slot.value, first, sum); return 1; }
  std::println(\"ok\");
  return 0;
}
""",
}


def cells():
    for container, spec in CONTAINERS.items():
        for access in spec[5]:
            yield f"{container}_{access}", program(container, access)
    for name, text in SCENARIOS.items():
        yield name, PRELUDE + text


# A deliberately broken variant the oracle must flag (the second element is read one position late):
# cell, original text, replacement. `--negative-controls` builds it.
NEGATIVE_CONTROL = ('vector_int_first_after_advance',
                    '  ++it;\n  auto& second',
                    '  ++it;\n  ++it;\n  auto& second')
