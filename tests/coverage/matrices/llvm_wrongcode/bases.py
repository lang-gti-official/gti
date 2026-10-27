"""Matrix 5: concrete-base hierarchy objects (owned fields in each class,
initialized locally, never forwarded into a base) x operation."""
import itertools

from .transfer import PRELUDE, TEXT, text_sum, TEXT_CHECK

COMMON = """#include <std/utility>
int64_t check_text(std::string& body) {
""" + TEXT_CHECK.replace("BODY", "body") + """
}
int64_t check_vec(std::vector<int32_t>& v) {
  mut int64_t sum = 0;
  for (mut std::size_t k = std::size_t(0); k < v.size(); k += std::size_t(1)) sum += int64_t(v[k]) * (int64_t(k) + 1);
  return int64_t(v.size()) * 1000 + sum;
}
std::vector<int32_t> make_vec(int32_t seed) {
  mut std::vector<int32_t> v{};
  for (mut int32_t k = 0; k < 3; k += 1) v.push_back(seed * 10 + k);
  return v;
}
"""

def vec_sum(seed):
    return 3 * 1000 + sum((seed * 10 + k) * (k + 1) for k in range(3))


KINDS = {
    "two_level": ("""class Base {
public:
  std::string text;
  int32_t seed;
  Base(int32_t seed) : text(std::string("TEXT")), seed(seed) {}
  int64_t base_check() { return check_text(this.text) + int64_t(this.seed); }
};
class Item : public Base {
public:
  Tracked mark;
  Item(int32_t seed) : Base(seed), mark(Tracked(seed)) {}
  int64_t check() { return this.base_check() * 3 + int64_t(this.mark.value()); }
};
""", lambda s: (text_sum(TEXT) + s) * 3 + s),
    "three_level": ("""class Base {
public:
  std::string text;
  Base(int32_t seed) : text(std::string("TEXT")) {}
};
class Mid : public Base {
public:
  std::vector<int32_t> values;
  Mid(int32_t seed) : Base(seed), values(make_vec(seed)) {}
};
class Item : public Mid {
public:
  std::unique_ptr<Cell> cell;
  Tracked mark;
  Item(int32_t seed) : Mid(seed), cell(std::make_unique<Cell>(seed * 2)), mark(Tracked(seed)) {}
  int64_t check() {
    if (this.cell == nullptr) return -1;
    return (check_text(this.text) * 3 + check_vec(this.values)) * 3 + int64_t(this.cell->value) + int64_t(this.mark.value());
  }
};
""", lambda s: (text_sum(TEXT) * 3 + vec_sum(s)) * 3 + s * 2 + s),
    "two_bases": ("""class Left {
public:
  std::string text;
  Left() : text(std::string("TEXT")) {}
};
class Right {
public:
  std::vector<int32_t> values;
  Tracked mark;
  Right(int32_t seed) : values(make_vec(seed)), mark(Tracked(seed)) {}
};
class Item : public Left, public Right {
public:
  int32_t seed;
  Item(int32_t seed) : Left(), Right(seed), seed(seed) {}
  int64_t check() { return (check_text(this.text) * 3 + check_vec(this.values)) * 3 + int64_t(this.mark.value()) + int64_t(this.seed); }
};
""", lambda s: (text_sum(TEXT) * 3 + vec_sum(s)) * 3 + s + s),
}

OPS = {
    "move": ("", """  mut Item a = Item(2);
  mut Item b = std::move(a);
  int64_t got = b.check();""", lambda X: X(2)),
    "byvalue": ("int64_t take(Item item) { return item.check(); }\n", """  mut Item a = Item(2);
  int64_t got = take(std::move(a)) * 3 + take(Item(3));""", lambda X: X(2) * 3 + X(3)),
    "return": ("""Item build(int32_t seed) {
  mut Item local = Item(seed);
  return local;
}
""", """  mut Item a = build(4);
  int64_t got = a.check();""", lambda X: X(4)),
    "vector": ("", """  mut std::vector<Item> items{};
  for (mut int32_t i = 1; i < 4; i += 1) items.push_back(Item(i));
  Item taken = std::exchange(items[std::size_t(1)], Item(5));
  std::swap(items[std::size_t(0)], items[std::size_t(2)]);
  mut int64_t got = 0;
  for (mut std::size_t k = std::size_t(0); k < items.size(); k += std::size_t(1)) got = got * 3 + items[k].check();
  mut Item kept = std::move(taken);
  got = got * 3 + kept.check();""", lambda X: ((X(3) * 3 + X(5)) * 3 + X(1)) * 3 + X(2)),
    "array": ("", """  mut Item[3] items = {Item(1), Item(2), Item(3)};
  std::swap(items[std::size_t(0)], items[std::size_t(2)]);
  mut int64_t got = 0;
  for (mut std::size_t k = std::size_t(0); k < std::size_t(3); k += std::size_t(1)) got = got * 3 + items[k].check();""",
              lambda X: (X(3) * 3 + X(2)) * 3 + X(1)),
    "replace": ("", """  mut Item a = Item(1);
  a = Item(4);
  a = Item(5);
  int64_t got = a.check();""", lambda X: X(5)),
    "field": ("""class Outer {
public:
  Item item;
  Outer(Item item) : item(std::move(item)) {}
};
""", """  mut Outer outer = Outer(Item(2));
  mut Outer moved = std::move(outer);
  int64_t got = moved.item.check();""", lambda X: X(2)),
    "swap_locals": ("", """  mut Item a = Item(1);
  mut Item b = Item(2);
  std::swap(a, b);
  int64_t got = a.check() * 3 + b.check();""", lambda X: X(2) * 3 + X(1)),
    "lambda": ("", """  mut Item a = Item(2);
  auto reader = [held = std::move(a)]() -> int64_t {
    mut int64_t total = 0;
    total += held.check();
    return total;
  };
  int64_t got = reader();""", lambda X: X(2)),
    "thread": ("", """  mut Item a = Item(2);
  mut std::thread_result<int64_t> worker{};
  if (!worker.start([held = std::move(a)]() -> int64_t { return held.check(); })) return 6;
  auto joined = worker.join();
  if (!joined) return 7;
  int64_t got = std::move(joined).value();""", lambda X: X(2)),
    "unique": ("", """  std::unique_ptr<Item> owner = std::make_unique<Item>(Item(3));
  std::unique_ptr<Item> moved = std::move(owner);
  int64_t got = moved->check();""", lambda X: X(3)),
}


def program(kind, op):
    kind_source, X = KINDS[kind]
    helpers, body, want = OPS[op]
    source = PRELUDE + COMMON + kind_source.replace("TEXT", TEXT) + "\n" + helpers + "\n"
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
    for kind, op in itertools.product(KINDS, OPS):
        yield f"{kind}__{op}", program(kind, op)
