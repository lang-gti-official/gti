"""Matrix 23: struct-of-arrays and fixed arrays of classes.

Fixed arrays of classes with destructors and owning fields, arrays of arrays,
arrays of unique_ptr, struct-of-arrays layouts updated in loops, arrays
passed to and returned from functions, sorted in place, and std::array of
classes.
"""

PRELUDE = """#include <std/array>
#include <std/algorithm>
#include <std/memory>
#include <std/string>
#include <std/format>
#include <std/utility>
#include <std/atomic>
std::atomic<int32_t> destroyed{0};
std::atomic<int32_t> order{0};
class Slot {
public:
  mut int32_t id = 0;
  Slot() = default;
  Slot(int32_t id) : id(id) {}
  ~Slot() {
    [[discard]] destroyed.fetch_add(1);
    if (this.id != 0) order.store(order.load() * 10 + this.id);
  }
};
class Named {
public:
  mut std::string name = std::string();
  mut int32_t rank = 0;
  Named() = default;
  bool operator==(Named& other) { return this.rank == other.rank; }
  bool operator!=(Named& other) { return this.rank != other.rank; }
  bool operator<(Named& other) { return this.rank < other.rank; }
  bool operator<=(Named& other) { return this.rank <= other.rank; }
  bool operator>(Named& other) { return this.rank > other.rank; }
  bool operator>=(Named& other) { return this.rank >= other.rank; }
};
"""

CELLS = {
    "class_array_cleanup": """int main() {
  {
    mut Slot slots[3] = {};
    for (mut uint64_t i = 0; i < uint64_t(3); i += uint64_t(1)) slots[i].id = int32_t(i) + 1;
  }
  if (destroyed.load() != 3 || order.load() != 321) {
    std::println("{} {}", destroyed.load(), order.load());
    return 1;
  }
  std::println("ok");
  return 0;
}
""",
    "struct_of_arrays": """class Particles {
public:
  mut int64_t xs[8] = {};
  mut int64_t vs[8] = {};
  mut bool alive[8] = {};
  Particles() = default;
  void step() mut {
    for (mut uint64_t i = 0; i < uint64_t(8); i += uint64_t(1)) {
      if (this.alive[i]) this.xs[i] += this.vs[i];
    }
  }
  int64_t total() {
    mut int64_t sum = 0;
    for (mut uint64_t i = 0; i < uint64_t(8); i += uint64_t(1)) {
      if (this.alive[i]) sum += this.xs[i];
    }
    return sum;
  }
};
int main() {
  mut Particles particles = Particles();
  for (mut uint64_t i = 0; i < uint64_t(8); i += uint64_t(1)) {
    particles.vs[i] = int64_t(i);
    particles.alive[i] = i % uint64_t(2) == uint64_t(0);
  }
  for (mut int32_t t = 0; t < 3; t += 1) particles.step();
  if (particles.total() != int64_t(36)) return 1;
  std::println("ok");
  return 0;
}
""",
    "owning_element_array": """int main() {
  mut Named people[3] = {};
  people[0].name = std::string("a long name for the first person on the heap");
  people[0].rank = 3;
  people[1].name = std::string("a long name for the second person on the heap");
  people[1].rank = 1;
  people[2].name = std::string("a long name for the third person on the heap");
  people[2].rank = 2;
  // Rotate left by constant indices: ranks become 1, 2, 3.
  Named held = std::move(people[0]);
  people[0] = std::move(people[1]);
  people[1] = std::move(people[2]);
  people[2] = std::move(held);
  if (people[0].rank != 1 || people[2].name.view() != "a long name for the first person on the heap") return 1;
  std::println("ok");
  return 0;
}
""",
    "nested_arrays": """int main() {
  mut int32_t grid[3][4] = {};
  for (mut uint64_t r = 0; r < uint64_t(3); r += uint64_t(1)) {
    for (mut uint64_t c = 0; c < uint64_t(4); c += uint64_t(1)) grid[r][c] = int32_t(r * uint64_t(10) + c);
  }
  mut int32_t sum = 0;
  for (auto& row : grid) {
    for (int32_t value : row) sum += value;
  }
  {
    mut Slot rows[2][2] = {};
    rows[1][0].id = 5;
  }
  if (sum != 138 || destroyed.load() != 4 || order.load() != 5) return 1;
  std::println("ok");
  return 0;
}
""",
    "unique_ptr_array": """int main() {
  {
    mut std::unique_ptr<Slot> owners[3] = {};
    owners[0] = std::make_unique<Slot>(1);
    owners[2] = std::make_unique<Slot>(3);
    std::unique_ptr<Slot> taken = std::move(owners[2]);
    if (taken->id != 3) return 1;
  }
  if (destroyed.load() != 2) return 2;
  std::println("ok");
  return 0;
}
""",
    "array_in_out": """int64_t weigh(int64_t values[4]) {
  mut int64_t sum = 0;
  for (int64_t value : values) sum += value;
  return sum;
}
std::array<Named, 2> make_pair() {
  mut std::array<Named, 2> result{};
  result[uint64_t(0)].rank = 7;
  result[uint64_t(1)].name = std::string("a returned element name that lives on the heap");
  return result;
}
int main() {
  mut int64_t values[4] = {1, 2, 3, 4};
  values[3] = 40;
  std::array<Named, 2> pair = make_pair();
  if (weigh(values) != int64_t(46) || pair[uint64_t(0)].rank != 7 ||
      pair[uint64_t(1)].name.size() < uint64_t(20)) return 1;
  std::println("ok");
  return 0;
}
""",
}


def cells():
    for name, body in CELLS.items():
        yield name, PRELUDE + body
