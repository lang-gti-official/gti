"""Matrix 6: payload enum shapes x operations, self-checking."""
import itertools

from .transfer import PRELUDE, TEXT, text_sum, TEXT_CHECK

COMMON = """import std.utility;
int64_t check_text(std::string& body) {
""" + TEXT_CHECK.replace("BODY", "body") + """
}
"""

# Each shape defines Msg with variants holding owned data, make(i) and digest(Msg&).
SHAPES = {
    "mixed": ("""enum Msg { text(std::string body), track(Tracked item, int32_t extra), cell(std::unique_ptr<Cell> c), empty };
Msg make(int32_t i) {
  if (i % 4 == 0) return Msg::text(std::string("TEXT"));
  if (i % 4 == 1) return Msg::track(Tracked(i), i * 2);
  if (i % 4 == 2) return Msg::cell(std::make_unique<Cell>(i * 3));
  return Msg::empty;
}
int64_t digest(Msg& m) {
  switch (m) {
  case Msg::text(auto& body): return check_text(body);
  case Msg::track(auto& item, auto& extra): return int64_t(item.value()) * 5 + int64_t(extra);
  case Msg::cell(auto& c): return c == nullptr ? int64_t(-1) : int64_t(c->value) * 7;
  case Msg::empty: return 11;
  }
}
""", lambda i: [text_sum(TEXT), i * 5 + i * 2, i * 3 * 7, 11][i % 4]),
    "nested": ("""enum Inner { held(Tracked item), text(std::string body) };
enum Msg { wrap(Inner inner, int32_t tag), empty };
Msg make(int32_t i) {
  if (i % 3 == 0) return Msg::wrap(Inner::held(Tracked(i)), i);
  if (i % 3 == 1) return Msg::wrap(Inner::text(std::string("TEXT")), i);
  return Msg::empty;
}
int64_t digest_inner(Inner& inner) {
  switch (inner) {
  case Inner::held(auto& item): return int64_t(item.value()) * 13;
  case Inner::text(auto& body): return check_text(body);
  }
}
int64_t digest(Msg& m) {
  switch (m) {
  case Msg::wrap(auto& inner, auto& tag): return digest_inner(inner) * 3 + int64_t(tag);
  case Msg::empty: return 11;
  }
}
""", lambda i: [i * 13 * 3 + i, text_sum(TEXT) * 3 + i, 11][i % 3]),
    "generic": ("""enum Maybe<T> { some(T value), none };
using Msg = Maybe<std::string>;
Msg make(int32_t i) {
  if (i % 2 == 0) return Msg::some(std::string("TEXT"));
  return Msg::none;
}
int64_t digest(Msg& m) {
  switch (m) {
  case Msg::some(auto& value): return check_text(value);
  case Msg::none: return 11;
  }
}
""", lambda i: [text_sum(TEXT), 11][i % 2]),
}

OPS = {
    "reassign": ("", """  mut Msg m = make(0);
  mut int64_t got = digest(m);
  for (mut int32_t i = 1; i < 6; i += 1) {
    m = make(i);
    got = got * 3 + digest(m);
  }""", lambda X: X(0)*3**5 + X(1)*3**4 + X(2)*3**3 + X(3)*3**2 + X(4)*3 + X(5)),
    "ternary": ("", """  mut int64_t got = 0;
  for (mut int32_t i = 0; i < 4; i += 1) {
    mut Msg m = i % 2 == 0 ? make(i) : make(i + 1);
    got = got * 3 + digest(m);
  }""", lambda X: ((X(0) * 3 + X(2)) * 3 + X(2)) * 3 + X(4)),
    "vector": ("", """  mut std::vector<Msg> items{};
  for (mut int32_t i = 0; i < 5; i += 1) items.push_back(make(i));
  Msg taken = std::exchange(items[std::size_t(1)], make(6));
  std::swap(items[std::size_t(0)], items[std::size_t(4)]);
  mut int64_t got = 0;
  for (mut std::size_t k = std::size_t(0); k < items.size(); k += std::size_t(1)) got = got * 3 + digest(items[k]);
  mut Msg kept = std::move(taken);
  got = got * 3 + digest(kept);""", lambda X: ((((X(4) * 3 + X(6)) * 3 + X(2)) * 3 + X(3)) * 3 + X(0)) * 3 + X(1)),
    "return_chain": ("""Msg relay(int32_t i) {
  mut Msg local = make(i);
  if (i > 100) return make(0);
  return local;
}
Msg relay_twice(int32_t i) { return relay(i); }
""", """  mut int64_t got = 0;
  for (mut int32_t i = 0; i < 4; i += 1) {
    mut Msg m = relay_twice(i);
    got = got * 3 + digest(m);
  }""", lambda X: ((X(0) * 3 + X(1)) * 3 + X(2)) * 3 + X(3)),
    "byvalue": ("int64_t take(Msg m) { return digest(m); }\n", """  mut int64_t got = 0;
  for (mut int32_t i = 0; i < 4; i += 1) {
    mut Msg m = make(i);
    got = got * 3 + take(std::move(m));
  }""", lambda X: ((X(0) * 3 + X(1)) * 3 + X(2)) * 3 + X(3)),
    "field": ("""class Envelope {
public:
  Msg msg;
  int32_t id;
  Envelope(Msg msg, int32_t id) : msg(std::move(msg)), id(id) {}
};
""", """  mut int64_t got = 0;
  for (mut int32_t i = 0; i < 4; i += 1) {
    mut Envelope e = Envelope(make(i), i);
    mut Envelope moved = std::move(e);
    got = got * 3 + digest(moved.msg) + int64_t(moved.id) - int64_t(i);
  }""", lambda X: ((X(0) * 3 + X(1)) * 3 + X(2)) * 3 + X(3)),
    "lambda": ("", """  mut int64_t got = 0;
  for (mut int32_t i = 0; i < 3; i += 1) {
    mut Msg m = make(i);
    auto reader = [held = std::move(m)]() -> int64_t {
      mut int64_t total = 0;
      total += digest(held);
      return total;
    };
    got = got * 3 + reader();
  }""", lambda X: (X(0) * 3 + X(1)) * 3 + X(2)),
    "array": ("", """  mut Msg[4] items = {make(0), make(1), make(2), make(3)};
  std::swap(items[std::size_t(0)], items[std::size_t(3)]);
  items[std::size_t(1)] = make(4);
  mut int64_t got = 0;
  for (mut std::size_t k = std::size_t(0); k < std::size_t(4); k += std::size_t(1)) got = got * 3 + digest(items[k]);""",
              lambda X: ((X(3) * 3 + X(4)) * 3 + X(2)) * 3 + X(0)),
}


def program(shape, op):
    shape_source, X = SHAPES[shape]
    helpers, body, want = OPS[op]
    source = PRELUDE + COMMON + shape_source.replace("TEXT", TEXT) + "\n" + helpers + "\n"
    source += f"""int32_t run() {{
{body}
  if (got != {want(X)}) {{
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
    return source


def cells():
    for shape, op in itertools.product(SHAPES, OPS):
        yield f"{shape}__{op}", program(shape, op)
