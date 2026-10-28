"""Matrix 16: algorithms over vectors of classes x element kind.

Sorting with a comparator, searching, counting, predicates, visiting with a
mutable capture, reversing, transforming and taking the largest element over
passive, owning and polymorphic elements built from ids 4, 1, 5, 2, 3.
"""
import itertools

PRELUDE = """#include <std/memory>
#include <std/vector>
#include <std/string>
#include <std/algorithm>
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
"""

IDS = [4, 1, 5, 2, 3]

ELEMENTS = {
    "point": ("""class Point {
public:
  int32_t x;
  int32_t y;
  Point(int32_t x, int32_t y) : x(x), y(y) {}
  bool operator<(Point& other) { return this.x < other.x; }
  bool operator==(Point& other) { return this.x == other.x && this.y == other.y; }
};
using T = Point;
T make(int32_t i) { return Point(i, i * 2); }
int64_t key(T& value) { return int64_t(value.x) * 10 + int64_t(value.y); }
T doubled(T& value) { return Point(value.x * 2, value.y); }
""", lambda i: i * 10 + i * 2, lambda i: (i * 2) * 10 + i * 2, True),
    "item": ("""class Item {
public:
  std::string name;
  Tracked mark;
  int32_t rank;
  Item(int32_t rank) : name(std::string("an item name long enough")), mark(Tracked(rank)), rank(rank) {}
  bool operator<(Item& other) { return this.rank < other.rank; }
};
using T = Item;
T make(int32_t i) { return Item(i); }
int64_t key(T& value) { return int64_t(value.rank) * 100 + int64_t(value.mark.value()); }
T doubled(T& value) { return Item(value.rank * 2); }
""", lambda i: i * 100 + i, lambda i: (i * 2) * 100 + i * 2, True),
    "poly": ("""interface Shape {
  int64_t score();
};
class Square : public Shape {
  Tracked mark;
public:
  Square(int32_t side) : mark(Tracked(side)) {}
  int64_t score() override { return int64_t(this.mark.value()) * 7; }
};
using T = std::unique_ptr<Shape>;
T make(int32_t i) { return std::upcast_unique<Shape, Square>(std::make_unique<Square>(i)); }
int64_t key(T& value) { return value->score(); }
T doubled(T& value) { return std::upcast_unique<Shape, Square>(std::make_unique<Square>(int32_t(value->score() / 7) * 2)); }
""", lambda i: i * 7, lambda i: i * 2 * 7, False),
}


def fold(values):
    total = 0
    for value in values:
        total = total * 1000 + value
    return total


ALGORITHMS = {
    "sort": ("""  std::sort(items, [](T& left, T& right) -> bool { return key(left) < key(right); });
  mut int64_t got = 0;
  for (auto& item : items) got = got * 1000 + key(item);""",
             lambda K, D: fold(sorted(K(i) for i in IDS))),
    "find_if": ("""  auto found = std::find_if(items.begin(), items.end(), [](T& value) -> bool { return key(value) > KEY3; });
  int64_t got = found != items.end() ? key(*found) : int64_t(-1);""",
                lambda K, D: next(K(i) for i in IDS if K(i) > K(3))),
    "count_if": ("""  int64_t got = int64_t(std::count_if(items, [](T& value) -> bool { return key(value) % 2 == 0; }));""",
                 lambda K, D: sum(1 for i in IDS if K(i) % 2 == 0)),
    "predicates": ("""  bool any = std::any_of(items, [](T& value) -> bool { return key(value) == KEY5; });
  bool all = std::all_of(items, [](T& value) -> bool { return key(value) > 0; });
  bool none = std::none_of(items, [](T& value) -> bool { return key(value) < 0; });
  int64_t got = (any ? 100 : 0) + (all ? 10 : 0) + (none ? 1 : 0);""",
                   lambda K, D: 111),
    "for_each": ("""  mut int64_t got = 0;
  [[discard]] std::for_each(items, [mut &got](T& value) -> void { got = got * 1000 + key(value); });""",
                 lambda K, D: fold(K(i) for i in IDS)),
    "reverse": ("""  std::reverse(items);
  mut int64_t got = 0;
  for (auto& item : items) got = got * 1000 + key(item);""",
                lambda K, D: fold(K(i) for i in reversed(IDS))),
    "transform": ("""  mut std::vector<T> output{};
  for (mut std::size_t k = std::size_t(0); k < items.size(); k += std::size_t(1)) output.push_back(make(0));
  std::transform(items, output, [](T& value) -> T { return doubled(value); });
  mut int64_t got = 0;
  for (auto& item : output) got = got * 1000 + key(item);""",
                  lambda K, D: fold(D(i) for i in IDS)),
    "max_element": ("""  auto best = std::max_element(items.begin(), items.end());
  int64_t got = key(*best);""", lambda K, D: max(K(i) for i in IDS)),
}


def program(element, algorithm):
    source, K, D, ordered = ELEMENTS[element]
    if algorithm == "max_element" and not ordered:
        return None
    body, want = ALGORITHMS[algorithm]
    body = body.replace("KEY3", str(K(3))).replace("KEY5", str(K(5)))
    fill = "".join(f"  items.push_back(make({i}));\n" for i in IDS)
    return PRELUDE + source + f"""int32_t run() {{
  mut std::vector<T> items{{}};
{fill}{body}
  if (got != {want(K, D)}) {{
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
    for element, algorithm in itertools.product(ELEMENTS, ALGORITHMS):
        source = program(element, algorithm)
        if source is not None:
            yield f"{element}__{algorithm}", source
