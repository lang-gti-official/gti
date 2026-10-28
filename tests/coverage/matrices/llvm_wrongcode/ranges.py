"""Matrix 20: iterators and ranges written by users.

Value-yielding and reference-yielding iterators, distinct sentinels,
iterators that keep a reference to their container, mutable traversal,
nested traversal, generic algorithms over user ranges, and ranges of owning
elements.
"""

PRELUDE = """#include <std/vector>
#include <std/iterator>
#include <std/string>
#include <std/format>
#include <std/utility>
#include <std/memory>
#include <std/atomic>
std::atomic<int32_t> destroyed{0};
class Count {
  mut int32_t current;
public:
  Count(int32_t start) : current(start) {}
  int32_t operator*() { return this.current; }
  void operator++() mut { this.current += 1; }
  bool operator!=(int32_t end) { return this.current != end; }
};
class Span {
  int32_t first;
  int32_t last;
public:
  Span(int32_t first, int32_t last) : first(first), last(last) {}
  Count begin() { return Count(this.first); }
  int32_t end() { return this.last; }
};
class Item {
public:
  int32_t id;
  std::string label;
  Item(int32_t id, std::string label) : id(id), label(std::move(label)) {}
  Item(Item& other) = delete;
  Item(Item&& other) = default;
  ~Item() { [[discard]] destroyed.fetch_add(1); }
};
class ShelfEnd {
public:
  uint64_t size;
  ShelfEnd(uint64_t size) : size(size) {}
};
class ShelfCursor {
  std::vector<Item>& items;
  mut uint64_t index;
public:
  ShelfCursor(std::vector<Item>& items) : items(items), index(uint64_t(0)) {}
  Item& operator*() { return this.items[this.index]; }
  void operator++() mut { this.index += 1; }
  bool operator!=(ShelfEnd end) { return this.index != end.size; }
};
class Shelf {
  mut std::vector<Item> items = std::vector<Item>();
public:
  Shelf() = default;
  void add(int32_t id) mut {
    this.items.push_back(Item(id, std::format("an item label long enough for the heap {}", id)));
  }
  ShelfCursor begin() { return ShelfCursor(this.items); }
  ShelfEnd end() { return ShelfEnd(this.items.size()); }
};
"""

CELLS = {
    "value_iterator": """int main() {
  mut int32_t sum = 0;
  for (int32_t value : Span(3, 7)) sum += value;
  Span named = Span(1, 4);
  for (auto& value : named) sum += value * 10;
  if (sum != 18 + 60) return 1;
  std::println("ok");
  return 0;
}
""",
    "container_cursor": """int main() {
  {
    mut Shelf shelf = Shelf();
    shelf.add(1);
    shelf.add(2);
    shelf.add(3);
    mut int32_t sum = 0;
    mut uint64_t letters = uint64_t(0);
    for (auto& item : shelf) {
      sum += item.id;
      letters += item.label.size();
    }
    if (sum != 6 || letters == uint64_t(0)) return 1;
  }
  if (destroyed.load() != 3) return 2;
  std::println("ok");
  return 0;
}
""",
    "nested_traversal": """int main() {
  mut Shelf shelf = Shelf();
  shelf.add(2);
  shelf.add(5);
  mut int32_t pairs = 0;
  for (auto& outer : shelf) {
    for (auto& inner : shelf) {
      if (outer.id < inner.id) pairs += outer.id * inner.id;
    }
  }
  if (pairs != 10) return 1;
  std::println("ok");
  return 0;
}
""",
    "explicit_loop_break": """int main() {
  mut Shelf shelf = Shelf();
  for (mut int32_t i = 1; i <= 5; i += 1) shelf.add(i);
  mut int32_t found = -1;
  for (auto& item : shelf) {
    if (item.id * item.id > 8) {
      found = item.id;
      break;
    }
  }
  mut int32_t total = 0;
  mut auto cursor = shelf.begin();
  ShelfEnd end = shelf.end();
  while (cursor != end) {
    total += (*cursor).id;
    ++cursor;
  }
  if (found != 3 || total != 15) return 1;
  std::println("ok");
  return 0;
}
""",
    "generic_over_user_range": """int64_t total<R>(R& range) requires std::range_of<R, int32_t> {
  mut int64_t sum = 0;
  for (auto& value : range) sum += int64_t(value);
  return sum;
}
int main() {
  Span span = Span(0, 5);
  mut std::vector<int32_t> values{};
  values.push_back(4);
  values.push_back(6);
  if (total(span) != 10 || total(values) != 10) return 1;
  std::println("ok");
  return 0;
}
""",
    "mutable_array_range": """class Row {
  mut int32_t[4] cells = {1, 2, 3, 4};
public:
  Row() = default;
  int32_t sum() {
    mut int32_t total = 0;
    for (int32_t value : this.cells) total += value;
    return total;
  }
  void scale(int32_t factor) mut {
    for (mut auto& value : this.cells) value *= factor;
  }
};
int main() {
  mut Row row = Row();
  row.scale(3);
  if (row.sum() != 30) return 1;
  std::println("ok");
  return 0;
}
""",
    "polymorphic_elements": """interface Shape {
  int32_t area();
};
class Square : public Shape {
  int32_t side;
public:
  Square(int32_t side) : side(side) {}
  int32_t area() override { return this.side * this.side; }
};
class ShapeEnd {
public:
  uint64_t size;
  ShapeEnd(uint64_t size) : size(size) {}
};
class ShapeCursor {
  std::vector<std::unique_ptr<Shape>>& shapes;
  mut uint64_t index = uint64_t(0);
public:
  ShapeCursor(std::vector<std::unique_ptr<Shape>>& shapes) : shapes(shapes) {}
  Shape& operator*() { return *this.shapes[this.index]; }
  void operator++() mut { this.index += uint64_t(1); }
  bool operator!=(ShapeEnd end) { return this.index != end.size; }
};
class Scene {
  mut std::vector<std::unique_ptr<Shape>> shapes = std::vector<std::unique_ptr<Shape>>();
public:
  Scene() = default;
  void add(int32_t side) mut { this.shapes.push_back(std::upcast_unique<Shape, Square>(std::make_unique<Square>(side))); }
  ShapeCursor begin() { return ShapeCursor(this.shapes); }
  ShapeEnd end() { return ShapeEnd(this.shapes.size()); }
};
int main() {
  mut Scene scene = Scene();
  scene.add(2);
  scene.add(3);
  mut int32_t total = 0;
  for (auto& shape : scene) total += shape.area();
  if (total != 13) return 1;
  std::println("ok");
  return 0;
}
""",
    "range_in_lambda": """int main() {
  mut Shelf shelf = Shelf();
  shelf.add(4);
  shelf.add(9);
  auto weigh = [&shelf]() -> int32_t {
    mut int32_t sum = 0;
    for (auto& item : shelf) sum += item.id;
    return sum;
  };
  if (weigh() + weigh() != 26) return 1;
  std::println("ok");
  return 0;
}
""",
    "through_owner_pointer": """class Box {
public:
  mut std::vector<int32_t> items = std::vector<int32_t>();
  Box() = default;
};
int32_t sum(std::unique_ptr<Box>& box) {
  mut int32_t total = 0;
  for (auto& item : box->items) total += item;
  return total;
}
int main() {
  mut std::unique_ptr<Box> box = std::make_unique<Box>();
  box->items.push_back(5);
  box->items.push_back(6);
  if (sum(box) != 11) return 1;
  std::println("ok");
  return 0;
}
""",
    "through_reference_member": """class Box {
public:
  mut std::vector<int32_t> items = std::vector<int32_t>();
  Box() = default;
};
int32_t sum(Box& box) {
  mut int32_t total = 0;
  for (auto& item : box.items) total += item;
  return total;
}
int main() {
  mut std::unique_ptr<Box> box = std::make_unique<Box>();
  box->items.push_back(5);
  box->items.push_back(6);
  if (sum(*box) != 11) return 1;
  std::println("ok");
  return 0;
}
""",
}


def cells():
    for name, body in CELLS.items():
        yield name, PRELUDE + body
