"""Matrix 13: optional, expected and payload enums of class and hierarchy
types x operation, self-checking.

wrap(i) produces a filled wrapper for i > 0 and an empty or error one
otherwise; inspect() digests it without consuming it, and extract() consumes
a filled wrapper and returns its value.
"""
import itertools

PRELUDE = """#include <std/memory>
#include <std/vector>
#include <std/optional>
#include <std/string>
#include <std/atomic>
#include <std/format>
#include <std/utility>
std::atomic<int32_t> constructed{0};
std::atomic<int32_t> destroyed{0};
class Tracked {
  int32_t id;
public:
  Tracked(int32_t id) : id(id) { [[discard]] constructed.fetch_add(1); }
  Tracked(Tracked& other) = delete;
  Tracked(Tracked&& other) = default;
  ~Tracked() { [[discard]] destroyed.fetch_add(1); }
  int32_t value() { return this.id; }
};
interface Shape {
  int64_t score();
};
class Square : public Shape {
  Tracked mark;
public:
  Square(int32_t side) : mark(Tracked(side)) {}
  int64_t score() override { return int64_t(this.mark.value()) * int64_t(this.mark.value()); }
};
class Record {
public:
  std::string name;
  Tracked mark;
  mut std::vector<int32_t> values;
  Record(int32_t i) : name(std::string("record name long enough")), mark(Tracked(i)) {
    this.values.push_back(i);
    this.values.push_back(i * 2);
  }
};
"""

TEXT = "a payload text long enough to own"

# Each value kind: (type, make(i) expression, digest(value) expression in v,
# expected digest of make(i)).
VALUES = {
    "text": ("std::string", f'std::string("{TEXT}")', "int64_t(v.size()) * 100",
             lambda i: len(TEXT) * 100),
    "tracked": ("Tracked", "Tracked(i)", "int64_t(v.value()) * 7", lambda i: i * 7),
    "poly_owner": ("std::unique_ptr<Shape>",
                   "std::upcast_unique<Shape, Square>(std::make_unique<Square>(i))",
                   "v->score()", lambda i: i * i),
    "record": ("Record", "Record(i)",
               "int64_t(v.name.size()) * 1000 + int64_t(v.mark.value()) * 10 + int64_t(v.values[std::size_t(1)])",
               lambda i: len("record name long enough") * 1000 + i * 10 + i * 2),
}

WRAPPERS = {
    "optional": ("""using W = std::optional<T>;
W wrap(int32_t i) {
  if (i > 0) return MAKE;
  return std::nullopt;
}
int64_t inspect(W& w) {
  if (!w.has_value()) return -1;
  T& v = w.value();
  return DIGEST;
}
T extract(W w) { return std::move(w).value(); }
"""),
    "expected": ("""using W = expected<T, std::string>;
W wrap(int32_t i) {
  if (i > 0) return MAKE;
  return unexpected(std::string("no value here"));
}
int64_t inspect(W& w) {
  if (!w.has_value()) return -int64_t(w.error().size());
  T& v = w.value();
  return DIGEST;
}
T extract(W w) { return std::move(w).value(); }
"""),
    "payload": ("""enum W { filled(T payload), failed(int32_t code) };
W wrap(int32_t i) {
  if (i > 0) return W::filled(MAKE);
  return W::failed(i - 5);
}
int64_t inspect(W& w) {
  switch (w) {
  case W::filled(auto& v): return DIGEST;
  case W::failed(auto& code): return int64_t(code);
  }
}
T extract(W w) {
  switch (std::move(w)) {
  case W::filled(v): return v;
  case W::failed(code): return MAKE_ZERO;
  }
}
"""),
}

EMPTY = {"optional": lambda i: -1, "expected": lambda i: -len("no value here"),
         "payload": lambda i: i - 5}

OPS = {
    "return_match": ("""  mut int64_t got = 0;
  for (mut int32_t i = -1; i <= 3; i += 1) {
    W w = wrap(i);
    got = got * 1000 + inspect(w);
  }""", lambda D, E: (((D(-1) * 1000 + D(0)) * 1000 + D(1)) * 1000 + D(2)) * 1000 + D(3)),
    "move_chain": ("""  mut W first = wrap(3);
  mut W second = std::move(first);
  mut W third = std::move(second);
  int64_t got = inspect(third);""", lambda D, E: D(3)),
    "store_field": ("""class Holder {
public:
  W held;
  int32_t tag;
  Holder(W held, int32_t tag) : held(std::move(held)), tag(tag) {}
};
FIELD_SPLIT  mut Holder holder = Holder(wrap(2), 1);
  mut Holder moved = std::move(holder);
  int64_t got = inspect(moved.held) * 10 + int64_t(moved.tag);""", lambda D, E: D(2) * 10 + 1),
    "vector": ("""  mut std::vector<W> items{};
  for (mut int32_t i = 0; i <= 3; i += 1) items.push_back(wrap(i));
  mut int64_t got = 0;
  for (auto& item : items) got = got * 1000 + inspect(item);""",
               lambda D, E: ((D(0) * 1000 + D(1)) * 1000 + D(2)) * 1000 + D(3)),
    "take_value": ("""  T v = extract(wrap(2));
  int64_t first = DIGEST;
  mut W kept = wrap(3);
  T w2 = extract(std::move(kept));
  int64_t got = first * 1000 + DIGEST2;""", lambda D, E: E(2) * 1000 + E(3)),
    "reassign": ("""  mut W w = wrap(1);
  w = wrap(0);
  w = wrap(3);
  int64_t got = inspect(w);""", lambda D, E: D(3)),
}


def program(value, wrapper, op):
    type_name, make, digest, expected_digest = VALUES[value]
    body, want = OPS[op]
    helpers = ""
    if "FIELD_SPLIT" in body:
        helpers, body = body.split("FIELD_SPLIT")
    wrapper_source = (WRAPPERS[wrapper].replace("MAKE_ZERO", make.replace("(i)", "(0)"))
                      .replace("MAKE", make).replace("DIGEST", digest))
    body = body.replace("DIGEST2", digest.replace("v.", "w2.").replace("v->", "w2->").replace("v[", "w2[")).replace("DIGEST", digest)
    empty = EMPTY[wrapper]

    def D(i):
        return expected_digest(i) if i > 0 else empty(i)

    return PRELUDE + f"using T = {type_name};\n" + wrapper_source + helpers + f"""int32_t run() {{
{body}
  if (got != {want(D, expected_digest)}) {{
    std::println("mismatch {{}}", got);
    return 2;
  }}
  return 0;
}}

int main() {{
  int32_t status = run();
  if (status != 0) return status;
  if (constructed.load() != destroyed.load()) {{
    std::println("constructed {{}} destroyed {{}}", constructed.load(), destroyed.load());
    return 9;
  }}
  std::println("ok");
  return 0;
}}
"""


def cells():
    for value, wrapper, op in itertools.product(VALUES, WRAPPERS, OPS):
        yield f"{value}__{wrapper}__{op}", program(value, wrapper, op)
