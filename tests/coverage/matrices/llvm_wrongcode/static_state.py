"""Matrix 26: static and global state, and constexpr values at run time.

Mutable globals and static members updated across calls, namespaced
globals, function-local counters through a static member, global owning
strings and vectors, constexpr functions and arrays whose results size and
drive runtime loops, constexpr values as generic arguments, and global
initialization order across dependent definitions.
"""

PRELUDE = """import std.atomic;
import std.meta;
import std.string;
import std.vector;
import std.format;
import std.utility;
"""

CELLS = {
    "atomic_globals": """std::atomic<int64_t> calls{int64_t(0)};
namespace stats {
  std::atomic<int64_t> total{int64_t(0)};
}
class Counter {
public:
  static int32_t base = 100;
  int32_t id;
  Counter(int32_t offset) : id(Counter::base + offset) {}
};
void record(int64_t value) {
  [[discard]] calls.fetch_add(int64_t(1));
  [[discard]] stats::total.fetch_add(value);
}
int main() {
  for (mut int64_t i = 1; i <= 10; i += 1) record(i);
  Counter c = Counter(2);
  if (calls.load() != 10 || stats::total.load() != 55 || c.id != 102) return 1;
  std::println("ok");
  return 0;
}
""",
    "static_functions": """static int64_t build_number = 24;
static int64_t adjusted(int64_t offset) { return build_number + offset; }
namespace release {
  static int64_t offset = 3;
  static int64_t stamp() { return adjusted(offset) * 2; }
}
class Limits {
public:
  static int64_t maximum = 9;
  static int64_t clamp(int64_t value) { return value > Limits::maximum ? Limits::maximum : value; }
};
int main() {
  mut int64_t sum = 0;
  for (mut int64_t i = 0; i < 20; i += 1) sum += Limits::clamp(i);
  if (release::stamp() != 54 || sum != 45 + 9 * 10) return 1;
  std::println("ok");
  return 0;
}
""",
    "constexpr_tables": """constexpr int64_t square(int64_t x) { return x * x; }
constexpr int64_t fib(int32_t n) {
  mut int64_t a = 0;
  mut int64_t b = 1;
  for (mut int32_t i = 0; i < n; i += 1) {
    int64_t next = a + b;
    a = b;
    b = next;
  }
  return a;
}
constexpr uint64_t width = uint64_t(square(int64_t(3)));
constexpr int64_t limit = fib(20);
int main() {
  mut int64_t cells[width] = {};
  for (mut uint64_t i = 0; i < width; i += uint64_t(1)) cells[i] = square(int64_t(i));
  mut int64_t sum = 0;
  for (int64_t value : cells) sum += value;
  mut int32_t n = 10;
  if (cells.size() != uint64_t(9) || sum != int64_t(204) || limit != int64_t(6765) ||
      fib(n) != int64_t(55)) return 1;
  std::println("ok");
  return 0;
}
""",
    "constexpr_generic_arguments": """constexpr uint64_t lanes = uint64_t(2) * uint64_t(4);
class Buffer<uint64_t N> {
public:
  mut int32_t data[N] = {};
  Buffer() = default;
  uint64_t capacity() { return N; }
};
uint64_t total_capacity<uint64_t A, uint64_t B>() { return A + B; }
int main() {
  mut Buffer<lanes> buffer = Buffer<lanes>();
  for (mut uint64_t i = 0; i < buffer.capacity(); i += uint64_t(1)) buffer.data[i] = int32_t(i);
  if (buffer.capacity() != uint64_t(8) || buffer.data[7] != 7 ||
      total_capacity<lanes, 3>() != uint64_t(11)) return 1;
  std::println("ok");
  return 0;
}
""",
    "dependent_global_init": """int64_t seed() { return 7; }
int64_t first = seed();
int64_t second = first * 3;
int64_t third = second + first;
int main() {
  if (first != 7 || second != 21 || third != 28) return 1;
  std::println("ok");
  return 0;
}
""",
    "constexpr_branching": """int64_t widen<T>(T value) requires std::integral<T> {
  if constexpr (std::meta::is_same<T, int64_t>()) {
    return value;
  } else {
    return int64_t(value) * int64_t(1000);
  }
}
int main() {
  if (widen(int32_t(4)) != int64_t(4000) || widen(int64_t(4)) != int64_t(4)) return 1;
  std::println("ok");
  return 0;
}
""",
}


def cells():
    for name, body in CELLS.items():
        yield name, PRELUDE + body
