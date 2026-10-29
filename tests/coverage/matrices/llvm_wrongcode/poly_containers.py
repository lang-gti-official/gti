"""Matrix 11: containers of polymorphic owners x operation, self-checking.

Each shape is a small hierarchy that owns cleanup-tracked state; owners are
`std::unique_ptr` or `std::shared_ptr` to the base, held in vectors and maps.
The checksum folds each element's virtual `score()` in container order, so a
lost, duplicated or misdispatched element changes it.
"""
import itertools

PRELUDE = """import std.memory;
import std.vector;
import std.map;
import std.unordered_map;
import std.algorithm;
import std.atomic;
import std.format;
import std.utility;
std::atomic<int32_t> constructed{0};
std::atomic<int32_t> destroyed{0};
[[unsafe_transfer]] class Tracked {
  int32_t id;
public:
  Tracked(int32_t id) : id(id) { [[discard]] constructed.fetch_add(1); }
  Tracked(Tracked& other) = delete;
  Tracked(Tracked&& other) = default;
  ~Tracked() { [[discard]] destroyed.fetch_add(1); }
  int32_t value() { return this.id; }
};
"""

# Each shape defines Shape (the base) and make(i), returning an owner of a
# derived object whose score is SCORE(i).
SHAPES = {
    "interface": ("""[[transfer]] interface Shape {
  int64_t score();
};
[[unsafe_transfer]] class Square : public Shape {
  Tracked mark;
  int32_t side;
public:
  Square(int32_t side) : mark(Tracked(side)), side(side) {}
  int64_t score() override { return int64_t(this.side) * int64_t(this.mark.value()); }
};
[[unsafe_transfer]] class Line : public Shape {
  Tracked mark;
public:
  Line(int32_t length) : mark(Tracked(length)) {}
  int64_t score() override { return int64_t(this.mark.value()) + 1000; }
};
""", lambda i: i * i if i % 2 == 0 else i + 1000),
    "class_base": ("""[[unsafe_transfer]] class Shape {
  Tracked base_mark;
public:
  Shape(int32_t id) : base_mark(Tracked(id)) {}
  virtual int64_t score() { return int64_t(this.base_mark.value()); }
  int32_t base_id() { return this.base_mark.value(); }
};
[[unsafe_transfer]] class Square : public Shape {
  int32_t side;
public:
  Square(int32_t side) : Shape(side), side(side) {}
  int64_t score() override { return int64_t(this.side) * int64_t(this.base_id()); }
};
[[unsafe_transfer]] class Line : public Shape {
public:
  Line(int32_t length) : Shape(length) {}
  int64_t score() override { return int64_t(this.base_id()) + 1000; }
};
""", lambda i: i * i if i % 2 == 0 else i + 1000),
}

UNIQUE_MAKE = """std::unique_ptr<Shape> make(int32_t i) {
  if (i % 2 == 0) return std::upcast_unique<Shape, Square>(std::make_unique<Square>(i));
  return std::upcast_unique<Shape, Line>(std::make_unique<Line>(i));
}
"""

SHARED_MAKE = """std::shared_ptr<Shape> make(int32_t i) {
  if (i % 2 == 0) return std::upcast_shared<Shape, Square>(std::make_shared<Square>(i));
  return std::upcast_shared<Shape, Line>(std::make_shared<Line>(i));
}
"""

# Ids 1..5 are inserted. Each operation returns (body, expected id order).
def fold(ids, score):
    total = 0
    for i in ids:
        total = (total * 31 + score(i)) % 1000000007
    return total


FOLD = """int64_t fold(int64_t total, int64_t score) { return (total * 31 + score) % 1000000007; }
"""

CONTAINERS = {
    "vector_unique": ("std::vector<std::unique_ptr<Shape>>", UNIQUE_MAKE),
    "vector_shared": ("std::vector<std::shared_ptr<Shape>>", SHARED_MAKE),
    "map_unique": ("std::map<int32_t, std::unique_ptr<Shape>>", UNIQUE_MAKE),
    "hash_shared": ("std::unordered_map<int32_t, std::shared_ptr<Shape>>", SHARED_MAKE),
}


def operation(container, op):
    vector = container.startswith("vector")
    fill = ("  for (mut int32_t i = 1; i <= 5; i += 1) items.push_back(make(i));\n" if vector
            else "  for (mut int32_t i = 1; i <= 5; i += 1) [[discard]] items.insert(i, make(i));\n")
    walk_vector = """  mut int64_t total = 0;
  for (auto& item : items) total = fold(total, item->score());
"""
    # Maps are walked by key so the order is the same for hashed maps.
    walk_map = """  mut int64_t total = 0;
  for (mut int32_t key = 1; key <= 9; key += 1) {
    if (items.contains(key)) total = fold(total, items.at(key)->score());
  }
"""
    walk = walk_vector if vector else walk_map
    if op == "iterate":
        return fill + walk, [1, 2, 3, 4, 5]
    if op == "erase":
        if vector:
            return fill + "  items.erase(std::size_t(1));\n" + walk, [1, 3, 4, 5]
        return fill + "  int32_t gone = 2;\n  if (items.erase(gone) != std::size_t(1)) return 3;\n" + walk, [1, 3, 4, 5]
    if op == "sort":
        if not vector:
            return None
        return (fill + """  std::sort(items, [](TYPE_ELEMENT& left, TYPE_ELEMENT& right) -> bool {
    return left->score() > right->score();
  });
""" + walk, "sorted")
    if op == "move":
        return (fill + """  mut CONTAINER moved = std::move(items);
  mut CONTAINER again = std::move(moved);
""" + walk.replace("items", "again"), [1, 2, 3, 4, 5])
    if op == "by_value":
        return (fill + """  int64_t total = consume(std::move(items));
""", [1, 2, 3, 4, 5])
    if op == "replace":
        if vector:
            return fill + "  items[std::size_t(2)] = make(7);\n" + walk, [1, 2, 7, 4, 5]
        return fill + "  [[discard]] items.insert_or_assign(3, make(7));\n" + walk, [1, 2, 7, 4, 5]
    if op == "clear_refill":
        return (fill + "  items.clear();\n" + fill.replace("i <= 5", "i <= 2") + walk, [1, 2])
    raise ValueError(op)


OPS = ["iterate", "erase", "sort", "move", "by_value", "replace", "clear_refill"]


def program(shape, container, op):
    shape_source, score = SHAPES[shape]
    type_name, make = CONTAINERS[container]
    vector = container.startswith("vector")
    result = operation(container, op)
    if result is None:
        return None
    body, order = result
    if order == "sorted":
        order = sorted([1, 2, 3, 4, 5], key=lambda i: -score(i))
    element = type_name[len("std::vector<"):-1] if vector else ""
    consume = ""
    if op == "by_value":
        walk = ("  for (auto& item : items) total = fold(total, item->score());\n" if vector else
                "  for (mut int32_t key = 1; key <= 9; key += 1) {\n"
                "    if (items.contains(key)) total = fold(total, items.at(key)->score());\n  }\n")
        consume = (f"int64_t consume({type_name} items) {{\n  mut int64_t total = 0;\n"
                   f"{walk}  return total;\n}}\n")
    body = body.replace("TYPE_ELEMENT", element).replace("CONTAINER", type_name)
    return PRELUDE + shape_source + make + FOLD + consume + f"""int32_t run() {{
  mut {type_name} items{{}};
{body}  if (total != {fold(order, score)}) {{
    std::println("mismatch {{}}", total);
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
    for shape, container, op in itertools.product(SHAPES, CONTAINERS, OPS):
        source = program(shape, container, op)
        if source is not None:
            yield f"{shape}__{container}__{op}", source
