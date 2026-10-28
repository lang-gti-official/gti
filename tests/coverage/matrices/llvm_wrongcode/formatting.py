"""Matrix 22: string building and formatting with user types.

User formatter specializations, including one that formats nested user
values and one for a class holding an owning string, used through
std::format, padding and repeated building of a long string, plus manual
string building in loops and through returned owners.
"""

PRELUDE = """#include <std/format>
#include <std/string>
#include <std/vector>
#include <std/utility>
class Point {
public:
  int32_t x;
  int32_t y;
  Point(int32_t x, int32_t y) : x(x), y(y) {}
};
class std::formatter<Point> {
public:
  expected<void, std::format_errc> format(Point& value, mut std::string& output,
                                          std::format_spec& spec) {
    auto text = std::format("({}, {})", value.x, value.y);
    output.append(text.view());
    return expected<void, std::format_errc>();
  }
};
class Label {
public:
  std::string name;
  Point at;
  Label(std::string name, Point at) : name(std::move(name)), at(at) {}
  Label(Label& other) = delete;
  Label(Label&& other) = default;
};
class LabelRef {
public:
  std::string& name;
  Point at;
  LabelRef(std::string& name, Point at) : name(name), at(at) {}
};
class std::formatter<LabelRef> {
public:
  expected<void, std::format_errc> format(LabelRef& value, mut std::string& output,
                                          std::format_spec& spec) {
    auto text = std::format("{}@{}", value.name.view(), value.at);
    output.append(text.view());
    return expected<void, std::format_errc>();
  }
};
bool same(std::string& text, std::string_view expected) {
  return text.view() == expected;
}
"""

CELLS = {
    "user_formatter": """int main() {
  Point p = Point(3, -4);
  auto text = std::format("p={} q={}", p, Point(0, 1));
  if (!same(text, "p=(3, -4) q=(0, 1)")) {
    std::println(text);
    return 1;
  }
  std::println("ok");
  return 0;
}
""",
    "nested_formatter": """int main() {
  Label label = Label(std::string("a long label name that will live on the heap"), Point(7, 8));
  auto text = std::format("[{}]", LabelRef(label.name, label.at));
  if (!same(text, "[a long label name that will live on the heap@(7, 8)]")) {
    std::println(text);
    return 1;
  }
  std::println("ok");
  return 0;
}
""",
    "build_in_loop": """int main() {
  mut std::string built = std::string();
  for (mut int32_t i = 0; i < 40; i += 1) {
    auto piece = std::format("{},", Point(i, i * i));
    built.append(piece.view());
  }
  auto tail = std::format("{}", Point(39, 1521));
  if (built.size() < uint64_t(200) || built[built.size() - uint64_t(1)] != ',') return 1;
  if (built.rfind(tail.view()) != built.size() - tail.size() - uint64_t(1)) return 2;
  std::println("ok");
  return 0;
}
""",
    "returned_owners": """std::string describe(std::vector<Label>& labels) {
  mut std::string out = std::string();
  for (auto& label : labels) {
    auto line = std::format("{};", LabelRef(label.name, label.at));
    out.append(line.view());
  }
  return out;
}
int main() {
  mut std::vector<Label> labels{};
  labels.push_back(Label(std::string("first long label stored on the heap"), Point(1, 2)));
  labels.push_back(Label(std::string("second long label stored on the heap"), Point(3, 4)));
  std::string text = describe(labels);
  if (!same(text, "first long label stored on the heap@(1, 2);second long label stored on the heap@(3, 4);")) {
    std::println(text);
    return 1;
  }
  std::println("ok");
  return 0;
}
""",
    "padding_and_numbers": """int main() {
  auto text = std::format("{:>6}|{:<4}|{:08}|{}", int32_t(42), int64_t(-7), uint32_t(255), true);
  if (!same(text, "    42|-7  |00000255|true")) {
    std::println(text);
    return 1;
  }
  std::println("ok");
  return 0;
}
""",
    "string_mutation_chain": """int main() {
  mut std::string text = std::string("prefix-");
  for (mut int32_t i = 0; i < 3; i += 1) text.append("abcdefghijklmnopqrstuvwxyz");
  text.replace(uint64_t(0), uint64_t(6), "PREFIX");
  text.insert(uint64_t(7), "[mid]");
  text.erase(uint64_t(text.size() - uint64_t(26)));
  if (!same(text, "PREFIX-[mid]abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz")) {
    std::println(text);
    return 1;
  }
  std::println("ok");
  return 0;
}
""",
}


def cells():
    for name, body in CELLS.items():
        yield name, PRELUDE + body
