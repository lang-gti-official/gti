"""Matrix 19: user operator overloading.

A value class with a complete comparison set, arithmetic and compound
assignment, used by sort, binary search, map keys, max_element and a
constrained generic, plus a subscript and call operator class.
"""

PRELUDE = """#include <std/vector>
#include <std/algorithm>
#include <std/map>
#include <std/string>
#include <std/format>
#include <std/utility>
#include <std/memory>
class Version {
public:
  mut int32_t major;
  mut int32_t minor;
  Version(int32_t major, int32_t minor) : major(major), minor(minor) {}
  int32_t compare(Version& other) {
    if (this.major != other.major) return this.major < other.major ? -1 : 1;
    if (this.minor != other.minor) return this.minor < other.minor ? -1 : 1;
    return 0;
  }
  bool operator==(Version& other) { return this.compare(other) == 0; }
  bool operator!=(Version& other) { return this.compare(other) != 0; }
  bool operator<(Version& other) { return this.compare(other) < 0; }
  bool operator<=(Version& other) { return this.compare(other) <= 0; }
  bool operator>(Version& other) { return this.compare(other) > 0; }
  bool operator>=(Version& other) { return this.compare(other) >= 0; }
  Version operator+(Version& other) {
    return Version(this.major + other.major, this.minor + other.minor);
  }
  void operator+=(Version& other) mut {
    this.major += other.major;
    this.minor += other.minor;
  }
  Version operator-() { return Version(-this.major, -this.minor); }
};
"""

CELLS = {
    "sort_search": """int main() {
  mut std::vector<Version> list{};
  list.push_back(Version(2, 1));
  list.push_back(Version(1, 9));
  list.push_back(Version(2, 0));
  list.push_back(Version(1, 9));
  std::sort(list);
  Version low = Version(1, 9);
  Version high = Version(2, 1);
  Version middle = Version(2, 0);
  Version absent = Version(5, 0);
  if (list[uint64_t(0)] != low || list[uint64_t(1)] != low || list[uint64_t(3)] != high) return 1;
  if (!std::binary_search(list.begin(), list.end(), middle)) return 2;
  if (std::binary_search(list.begin(), list.end(), absent)) return 3;
  std::println("ok");
  return 0;
}
""",
    "map_keys": """int main() {
  mut std::map<Version, std::string> names{};
  names.insert(Version(3, 0), std::string("c"));
  names.insert(Version(1, 0), std::string("a"));
  names.insert(Version(2, 5), std::string("b"));
  mut std::string order = std::string();
  for (auto& entry : names) order.append(entry.second.view());
  if (order.view() != "abc") return 1;
  std::println("ok");
  return 0;
}
""",
    "arithmetic": """int main() {
  mut Version total = Version(0, 0);
  Version step = Version(1, 2);
  for (mut int32_t i = 0; i < 3; i += 1) total += step;
  Version sum = total + step;
  Version neg = -sum;
  Version expected = Version(4, 8);
  if (sum != expected || neg.major != -4 || !(neg < sum) || !(sum >= total)) return 1;
  std::println("ok");
  return 0;
}
""",
    "generic_ordering": """bool later<T>(T& a, T& b) requires std::totally_ordered<T> {
  return a > b && !(a <= b);
}
int main() {
  mut std::vector<Version> list{};
  list.push_back(Version(1, 1));
  list.push_back(Version(4, 0));
  list.push_back(Version(3, 7));
  auto top = std::max_element(list.begin(), list.end());
  Version newest = Version(4, 0);
  if (*top != newest) return 1;
  Version a = Version(2, 2);
  Version b = Version(2, 3);
  if (!later(b, a) || later(a, b)) return 2;
  std::println("ok");
  return 0;
}
""",
    "subscript_call": """class Grid {
  mut int32_t cells[6] = {0, 0, 0, 0, 0, 0};
public:
  Grid() = default;
  int32_t& operator[](uint64_t index) { return this.cells[index]; }
  mut int32_t& operator[](uint64_t index) mut { return this.cells[index]; }
  int32_t operator()(uint64_t row, uint64_t column) { return this.cells[row * 3 + column]; }
};
int main() {
  mut Grid grid = Grid();
  for (mut uint64_t i = 0; i < 6; i += 1) grid[i] = int32_t(i) * 2;
  if (grid(uint64_t(1), uint64_t(2)) != 10 || grid[uint64_t(3)] != 6) return 1;
  std::println("ok");
  return 0;
}
""",
    "class_key_ordering_owner": """class Named {
public:
  Version version;
  std::string name;
  Named(Version version, std::string name) : version(version), name(std::move(name)) {}
  bool operator==(Named& other) { return this.version == other.version; }
  bool operator!=(Named& other) { return this.version != other.version; }
  bool operator<(Named& other) { return this.version < other.version; }
  bool operator<=(Named& other) { return this.version <= other.version; }
  bool operator>(Named& other) { return this.version > other.version; }
  bool operator>=(Named& other) { return this.version >= other.version; }
};
int main() {
  mut std::vector<Named> list{};
  list.push_back(Named(Version(3, 0), std::string("a long name that lives on the heap three")));
  list.push_back(Named(Version(1, 0), std::string("a long name that lives on the heap one")));
  list.push_back(Named(Version(2, 0), std::string("a long name that lives on the heap two")));
  std::sort(list);
  Version newest = Version(3, 0);
  if (list[uint64_t(0)].name.view() != "a long name that lives on the heap one" ||
      list[uint64_t(2)].version != newest) return 1;
  std::println("ok");
  return 0;
}
""",
    "generic_vector_ops": """class Vec2<std::numeric T> {
public:
  T x;
  T y;
  Vec2(T x, T y) : x(x), y(y) {}
  Vec2<T> operator+(Vec2<T>& other) { return Vec2<T>(T(this.x + other.x), T(this.y + other.y)); }
  Vec2<T> operator*(T scale) { return Vec2<T>(T(this.x * scale), T(this.y * scale)); }
  bool operator==(Vec2<T>& other) { return this.x == other.x && this.y == other.y; }
  bool operator!=(Vec2<T>& other) { return !(this == other); }
};
int main() {
  mut std::vector<Vec2<int64_t>> points{};
  for (mut int64_t i = 0; i < 5; i += 1) points.push_back(Vec2<int64_t>(i, i * i));
  mut Vec2<int64_t> sum = Vec2<int64_t>(int64_t(0), int64_t(0));
  for (auto& point : points) sum = sum + point;
  Vec2<int32_t> small = Vec2<int32_t>(2, 3) * 4;
  Vec2<int64_t> expected = Vec2<int64_t>(int64_t(10), int64_t(30));
  Vec2<int32_t> expected_small = Vec2<int32_t>(8, 12);
  if (sum != expected || small != expected_small) return 1;
  std::println("ok");
  return 0;
}
""",
    "map_of_owners": """class Res {
public:
  int32_t id;
  Res(int32_t id) : id(id) {}
};
int main() {
  mut std::map<Version, std::unique_ptr<Res>> registry{};
  for (mut int32_t i = 0; i < 6; i += 1) {
    registry.insert(Version(i % 3, i), std::make_unique<Res>(i));
  }
  Version probe = Version(1, 4);
  if (!registry.contains(probe) || registry.at(probe)->id != 4 || registry.size() != uint64_t(6)) return 1;
  mut int32_t order = 0;
  for (auto& entry : registry) order = order * 10 + entry.second->id;
  if (order != 31425) {
    std::println("{}", order);
    return 2;
  }
  std::println("ok");
  return 0;
}
""",
    "sort_with_comparator": """class Newest {
public:
  bool operator()(Version& left, Version& right) { return left > right; }
};
int main() {
  mut std::vector<Version> list{};
  for (mut int32_t i = 0; i < 8; i += 1) list.push_back(Version((i * 5) % 7, i));
  std::sort(list, Newest());
  for (mut uint64_t i = uint64_t(1); i < list.size(); i += uint64_t(1)) {
    if (list[i - uint64_t(1)] < list[i]) return 1;
  }
  mut int32_t keys = 0;
  std::sort(list, [](Version& a, Version& b) -> bool { return a.minor < b.minor; });
  for (auto& v : list) keys = keys * 10 + v.minor;
  if (keys != 1234567) return 2;
  std::println("ok");
  return 0;
}
""",
}


def cells():
    for name, body in CELLS.items():
        yield name, PRELUDE + body
