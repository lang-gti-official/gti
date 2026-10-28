"""Matrix 15: generic functions and classes over hierarchies x hierarchy.

Constrained generic functions, generic owners of polymorphic objects, a
generic class implementing an interface, generic factories and visitors, over
an interface hierarchy and a class hierarchy with virtual methods.
"""
import itertools

PRELUDE = """#include <std/memory>
#include <std/functional>
#include <std/vector>
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
concept scored<T> = requires(T& value) {
  { value.score() } -> int64_t;
};
concept valued<T> = requires(T& value) {
  { value.value() } -> int32_t;
};
"""

HIERARCHIES = {
    "interface": """interface Shape {
  int64_t score();
};
class Square : public Shape {
  Tracked mark;
public:
  Square(int32_t side) : mark(Tracked(side)) {}
  int64_t score() override { return int64_t(this.mark.value()) * int64_t(this.mark.value()); }
};
class Line : public Shape {
  Tracked mark;
public:
  Line(int32_t length) : mark(Tracked(length)) {}
  int64_t score() override { return int64_t(this.mark.value()) + 100; }
};
""",
    "class_base": """class Shape {
  Tracked base_mark;
public:
  Shape(int32_t id) : base_mark(Tracked(id)) {}
  virtual int64_t score() { return int64_t(this.base_mark.value()); }
  int32_t id() { return this.base_mark.value(); }
};
class Square : public Shape {
public:
  Square(int32_t side) : Shape(side) {}
  int64_t score() override { return int64_t(this.id()) * int64_t(this.id()); }
};
class Line : public Shape {
public:
  Line(int32_t length) : Shape(length) {}
  int64_t score() override { return int64_t(this.id()) + 100; }
};
""",
}

PATTERNS = {
    "constrained_sum": ("""int64_t total<T>(std::vector<T>& items) requires scored<T> && std::vector_element<T> {
  mut int64_t sum = 0;
  for (auto& item : items) sum = sum * 1000 + item.score();
  return sum;
}
""", """  mut std::vector<Square> squares{};
  for (mut int32_t i = 1; i <= 3; i += 1) squares.push_back(Square(i));
  int64_t got = total(squares);""", (1 * 1000 + 4) * 1000 + 9),
    "generic_factory": ("""std::unique_ptr<Shape> make_shape<T>(int32_t i) {
  return std::upcast_unique<Shape, T>(std::make_unique<T>(i));
}
""", """  mut std::vector<std::unique_ptr<Shape>> shapes{};
  shapes.push_back(make_shape<Square>(2));
  shapes.push_back(make_shape<Line>(3));
  mut int64_t got = 0;
  for (auto& shape : shapes) got = got * 1000 + shape->score();""", 4 * 1000 + 103),
    "generic_pool": ("""class Pool<T> {
public:
  mut std::vector<std::unique_ptr<T>> items;
  Pool() {}
  void add(std::unique_ptr<T> item) mut { this.items.push_back(std::move(item)); }
  int64_t sum() requires scored<T> {
    mut int64_t total = 0;
    for (auto& item : this.items) total = total * 1000 + item->score();
    return total;
  }
};
""", """  mut Pool<Shape> pool = Pool<Shape>();
  pool.add(std::upcast_unique<Shape, Square>(std::make_unique<Square>(3)));
  pool.add(std::upcast_unique<Shape, Line>(std::make_unique<Line>(4)));
  mut Pool<Shape> moved = std::move(pool);
  int64_t got = moved.sum();""", 9 * 1000 + 104),
    "generic_derived": ("""class Tagged<valued T> : public Shape {
  T payload;
  int32_t weight;
public:
  Tagged(T payload, int32_t weight) : SHAPE_INIT payload(std::move(payload)), weight(weight) {}
  int64_t score() override { return int64_t(this.payload.value()) * int64_t(this.weight); }
};
""", """  std::unique_ptr<Shape> owner = std::upcast_unique<Shape, Tagged<Tracked>>(
      std::make_unique<Tagged<Tracked>>(Tagged<Tracked>(Tracked(6), 7)));
  std::unique_ptr<Shape> moved = std::move(owner);
  int64_t got = moved->score();""", 42),
    "generic_best": ("""T& best<T>(std::vector<T>& items) requires scored<T> && std::vector_element<T> {
  mut std::size_t chosen = std::size_t(0);
  for (mut std::size_t i = std::size_t(1); i < items.size(); i += std::size_t(1)) {
    if (items[i].score() > items[chosen].score()) chosen = i;
  }
  return items[chosen];
}
""", """  mut std::vector<Line> lines{};
  for (mut int32_t i = 1; i <= 4; i += 1) lines.push_back(Line(i % 3));
  Line& top = best(lines);
  int64_t got = top.score();""", 102),
    "generic_visit": ("""void visit<F>(std::vector<std::unique_ptr<Shape>>& items, mut F f) {
  for (auto& item : items) f(*item);
}
""", """  mut std::vector<std::unique_ptr<Shape>> shapes{};
  for (mut int32_t i = 1; i <= 3; i += 1) shapes.push_back(std::upcast_unique<Shape, Square>(std::make_unique<Square>(i)));
  mut int64_t got = 0;
  visit(shapes, [mut &got](Shape& shape) -> void { got = got * 1000 + shape.score(); });""",
                      (1 * 1000 + 4) * 1000 + 9),
    "generic_apply": ("""int64_t apply<F>(Shape& shape, F f) requires std::read_callable<F, Shape, int64_t> {
  int64_t value = f(shape);
  return value * 10;
}
""", """  Square square = Square(5);
  int64_t got = apply(square, [](Shape& shape) -> int64_t { return shape.score() + 1; });""", 260),
}


def program(hierarchy, pattern):
    helpers, body, expected = PATTERNS[pattern]
    shape_init = "Shape(weight)," if hierarchy == "class_base" else ""
    helpers = helpers.replace("SHAPE_INIT ", shape_init + " " if shape_init else "")
    return PRELUDE + HIERARCHIES[hierarchy] + helpers + f"""int32_t run() {{
{body}
  if (got != {expected}) {{
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
    for hierarchy, pattern in itertools.product(HIERARCHIES, PATTERNS):
        yield f"{hierarchy}__{pattern}", program(hierarchy, pattern)
