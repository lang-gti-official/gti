"""Matrix 25: error handling with expected and optional chains.

Prefix `?` propagation through several layers with owning success values,
user error enums and owning error payloads, transform and transform_error,
and_then chains, optional of owning classes, early exits that must clean up
locals created before the failure, and loops that stop at the first error.
"""

PRELUDE = """import std.string;
import std.vector;
import std.optional;
import std.format;
import std.utility;
import std.atomic;
std::atomic<int32_t> live{0};
class Res {
  int32_t id;
public:
  Res(int32_t id) : id(id) { [[discard]] live.fetch_add(1); }
  Res(Res& other) = delete;
  Res(Res&& other) = default;
  ~Res() { [[discard]] live.fetch_sub(1); }
  int32_t value() { return this.id; }
};
enum class fault { missing, negative, overflow };
expected<int32_t, fault> parse(int32_t raw) {
  if (raw < 0) return unexpected(fault::negative);
  if (raw > 1000) return unexpected(fault::overflow);
  return raw;
}
"""

CELLS = {
    "propagate_layers": """expected<int32_t, fault> doubled(int32_t raw) {
  Res guard = Res(raw);
  int32_t value = ?parse(raw);
  return value * 2;
}
expected<int32_t, fault> summed(int32_t a, int32_t b) {
  Res outer = Res(0);
  int32_t x = ?doubled(a);
  int32_t y = ?doubled(b);
  return x + y;
}
int main() {
  auto good = summed(3, 4);
  auto bad = summed(3, -1);
  auto big = summed(2000, 1);
  if (!good || good.value() != 14 || bad || bad.error() != fault::negative ||
      big.error() != fault::overflow || live.load() != 0) return 1;
  std::println("ok");
  return 0;
}
""",
    "owning_values": """expected<std::string, fault> name_of(int32_t raw) {
  int32_t value = ?parse(raw);
  return std::format("a generated name long enough for the heap {}", value);
}
expected<Res, fault> resource(int32_t raw) {
  int32_t value = ?parse(raw);
  return Res(value);
}
int main() {
  {
    auto name = name_of(7);
    auto res = resource(9);
    auto failed = resource(-5);
    if (!name || name.value().size() < uint64_t(30) || !res || res.value().value() != 9 ||
        failed || live.load() != 1) return 1;
  }
  if (live.load() != 0) return 2;
  std::println("ok");
  return 0;
}
""",
    "owning_errors": """expected<int32_t, std::string> checked(int32_t raw) {
  if (raw % 2 != 0) return unexpected(std::format("an odd value error message on the heap {}", raw));
  return raw / 2;
}
expected<int32_t, std::string> chain(int32_t raw) {
  int32_t half = ?checked(raw);
  int32_t quarter = ?checked(half);
  return quarter;
}
int main() {
  auto fine = chain(8);
  auto odd = chain(6);
  if (!fine || fine.value() != 2 || odd || odd.error().view() != "an odd value error message on the heap 3") return 1;
  std::println("ok");
  return 0;
}
""",
    "transform_chains": """fault widen(fault f) { return f == fault::negative ? fault::missing : f; }
int32_t triple(int32_t v) { return v * 3; }
expected<int32_t, fault> halve(int32_t v) {
  if (v % 2 != 0) return unexpected(fault::missing);
  return v / 2;
}
int main() {
  auto a = parse(4).transform(triple);
  auto b = parse(-4).transform_error(widen);
  auto c = parse(6).and_then(halve).transform(triple);
  auto d = parse(5).and_then(halve);
  if (!a || a.value() != 12 || b || b.error() != fault::missing || !c || c.value() != 9 ||
      d || d.error() != fault::missing) return 1;
  std::println("ok");
  return 0;
}
""",
    "optional_owning": """std::optional<Res> maybe(int32_t id) {
  if (id < 0) return std::optional<Res>();
  return std::optional<Res>(Res(id));
}
int32_t total(std::vector<int32_t>& ids) {
  mut int32_t sum = 0;
  for (int32_t id : ids) {
    std::optional<Res> found = maybe(id);
    if (found) sum += found.value().value();
  }
  return sum;
}
int main() {
  mut std::vector<int32_t> ids{};
  ids.push_back(4);
  ids.push_back(-1);
  ids.push_back(6);
  if (total(ids) != 10 || live.load() != 0) return 1;
  {
    mut std::optional<Res> slot = maybe(3);
    slot = maybe(-3);
    if (slot || live.load() != 0) return 2;
  }
  std::println("ok");
  return 0;
}
""",
    "first_error_in_loop": """expected<int32_t, fault> sum_all(std::vector<int32_t>& raws) {
  mut int32_t sum = 0;
  for (int32_t raw : raws) {
    Res step = Res(raw);
    int32_t value = ?parse(raw);
    sum += value;
  }
  return sum;
}
int main() {
  mut std::vector<int32_t> good{};
  good.push_back(1);
  good.push_back(2);
  mut std::vector<int32_t> bad{};
  bad.push_back(5);
  bad.push_back(-2);
  bad.push_back(7);
  auto a = sum_all(good);
  auto b = sum_all(bad);
  if (!a || a.value() != 3 || b || b.error() != fault::negative || live.load() != 0) return 1;
  std::println("ok");
  return 0;
}
""",
}


def cells():
    for name, body in CELLS.items():
        yield name, PRELUDE + body
