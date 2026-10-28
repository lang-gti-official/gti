"""Matrix 21: nested generics and concepts.

Generic classes instantiated with other generic classes, owning and
movable payloads inside nested generics, concept-constrained classes and
functions over nested instances, generic containers of generic owners and
value-parameterized generics.
"""

PRELUDE = """#include <std/vector>
#include <std/memory>
#include <std/string>
#include <std/format>
#include <std/utility>
#include <std/atomic>
std::atomic<int32_t> live{0};
class Token {
  int32_t id;
public:
  Token(int32_t id) : id(id) { [[discard]] live.fetch_add(1); }
  Token(Token& other) = delete;
  Token(Token&& other) = default;
  ~Token() { [[discard]] live.fetch_sub(1); }
  int32_t value() { return this.id; }
};
concept valued<T> = requires(T& value) {
  { value.value() } -> int32_t;
};
class Cell<T> {
  mut T item;
public:
  Cell(T item) : item(std::move(item)) {}
  T& get() { return this.item; }
  mut T& get_mut() mut { return this.item; }
};
class Pair<A, B> {
public:
  A first;
  B second;
  Pair(A first, B second) : first(std::move(first)), second(std::move(second)) {}
};
class Scored<valued T> {
  T item;
  int32_t bonus;
public:
  Scored(T item, int32_t bonus) : item(std::move(item)), bonus(bonus) {}
  int32_t value() { return this.item.value() + this.bonus; }
};
"""

CELLS = {
    "cell_of_cell": """int main() {
  Cell<Cell<int32_t>> nested = Cell<Cell<int32_t>>(Cell<int32_t>(7));
  Cell<Cell<Cell<int64_t>>> deeper =
      Cell<Cell<Cell<int64_t>>>(Cell<Cell<int64_t>>(Cell<int64_t>(int64_t(9))));
  if (nested.get().get() != 7 || deeper.get().get().get() != int64_t(9)) return 1;
  std::println("ok");
  return 0;
}
""",
    "owning_nested": """int main() {
  {
    Cell<Pair<Token, std::string>> held = Cell<Pair<Token, std::string>>(
        Pair<Token, std::string>(Token(3), std::string("a label long enough to live on the heap")));
    mut Cell<Cell<Token>> moved = Cell<Cell<Token>>(Cell<Token>(Token(4)));
    Cell<Cell<Token>> taken = std::move(moved);
    if (held.get().first.value() + taken.get().get().value() != 7) return 1;
    if (live.load() != 2) return 2;
  }
  if (live.load() != 0) return 3;
  std::println("ok");
  return 0;
}
""",
    "concept_over_nested": """int32_t total<T>(std::vector<T>& values) requires valued<T> && std::vector_element<T> {
  mut int32_t sum = 0;
  for (auto& value : values) sum += value.value();
  return sum;
}
int main() {
  mut std::vector<Scored<Token>> list{};
  list.push_back(Scored<Token>(Token(1), 10));
  list.push_back(Scored<Token>(Token(2), 20));
  Scored<Scored<Token>> twice = Scored<Scored<Token>>(Scored<Token>(Token(3), 30), 300);
  if (total(list) != 33 || twice.value() != 333) return 1;
  std::println("ok");
  return 0;
}
""",
    "generic_of_unique": """class Stack<T> {
  mut std::vector<std::unique_ptr<T>> items = std::vector<std::unique_ptr<T>>();
public:
  Stack() = default;
  void push(T value) mut { this.items.push_back(std::make_unique<T>(std::move(value))); }
  uint64_t size() { return this.items.size(); }
  T& top() { return *this.items[this.items.size() - uint64_t(1)]; }
};
int main() {
  {
    mut Stack<Cell<Token>> stack = Stack<Cell<Token>>();
    stack.push(Cell<Token>(Token(5)));
    stack.push(Cell<Token>(Token(6)));
    mut Stack<Stack<int32_t>> outer = Stack<Stack<int32_t>>();
    outer.push(Stack<int32_t>());
    if (stack.size() != uint64_t(2) || stack.top().get().value() != 6 ||
        outer.size() != uint64_t(1)) return 1;
  }
  if (live.load() != 0) return 2;
  std::println("ok");
  return 0;
}
""",
    "value_parameters": """class Fixed<std::numeric T, uint64_t N> {
  mut T values[N] = {};
public:
  Fixed() = default;
  void set(uint64_t index, T value) mut { this.values[index] = value; }
  T sum() {
    mut T total = T(0);
    for (T value : this.values) total += value;
    return total;
  }
};
class Rows<uint64_t R, uint64_t C> {
public:
  mut Fixed<int64_t, C> rows[R] = {};
  Rows() = default;
};
int main() {
  mut Fixed<int64_t, 4> four = Fixed<int64_t, 4>();
  mut Rows<2, 3> grid = Rows<2, 3>();
  for (mut uint64_t i = 0; i < uint64_t(4); i += uint64_t(1)) four.set(i, int64_t(i) + int64_t(1));
  grid.rows[1].set(uint64_t(2), int64_t(5));
  if (four.sum() != int64_t(10) || grid.rows[1].sum() != int64_t(5)) return 1;
  std::println("ok");
  return 0;
}
""",
    "generic_method_chain": """R apply<T, R>(Cell<T>& cell, R seed) requires std::copyable<R> {
  return seed;
}
U convert<T, U>(Pair<T, U>& pair) requires std::copyable<U> { return pair.second; }
int main() {
  Pair<std::string, int64_t> pair = Pair<std::string, int64_t>(
      std::string("a key long enough for the heap"), int64_t(42));
  Cell<Pair<std::string, int64_t>> cell = Cell<Pair<std::string, int64_t>>(
      Pair<std::string, int64_t>(std::string("another long heap key"), int64_t(8)));
  if (convert(pair) + convert(cell.get()) != int64_t(50) || apply(cell, 3) != 3) return 1;
  std::println("ok");
  return 0;
}
""",
}


def cells():
    for name, body in CELLS.items():
        yield name, PRELUDE + body
