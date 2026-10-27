"""Matrix 2: owned element kind x container x operation, self-checking."""
import itertools

from .transfer import PRELUDE, TEXT, text_sum, TEXT_CHECK

ELEMENTS = {
    "string": dict(type="std::string",
                   make=lambda i: f'std::string("{TEXT}{i}")',
                   check="int64_t check_e(std::string& v) {\n" + TEXT_CHECK.replace("BODY", "v") + "\n}",
                   expected=lambda i: text_sum(TEXT + str(i))),
    "unique": dict(type="std::unique_ptr<Cell>",
                   make=lambda i: f"std::make_unique<Cell>({40 + i})",
                   check="int64_t check_e(std::unique_ptr<Cell>& v) { if (v == nullptr) return -1; return int64_t(v->value) * 3; }",
                   expected=lambda i: (40 + i) * 3),
    "tracked": dict(type="Tracked",
                    make=lambda i: f"Tracked({10 + i})",
                    check="int64_t check_e(Tracked& v) { return int64_t(v.value()) * 7; }",
                    expected=lambda i: (10 + i) * 7),
    "payload": dict(type="Packet",
                    make=lambda i: f'Packet::text(std::string("{TEXT}{i}"))',
                    check="int64_t check_text(std::string& body) {\n" + TEXT_CHECK.replace("BODY", "body") + "\n}\n" +
                          "int64_t check_e(Packet& v) {\n  switch (v) {\n  case Packet::text(auto& body): return check_text(body);\n  case Packet::none: return -1;\n  }\n}",
                    expected=lambda i: text_sum(TEXT + str(i))),
    "vector": dict(type="std::vector<int32_t>",
                   make=lambda i: f"make_vec({i})",
                   helper="std::vector<int32_t> make_vec(int32_t seed) {\n  mut std::vector<int32_t> v{};\n  for (mut int32_t k = 0; k < 4; k += 1) v.push_back(seed * 10 + k);\n  return v;\n}",
                   check="int64_t check_e(std::vector<int32_t>& v) {\n  mut int64_t sum = 0;\n  for (mut std::size_t k = std::size_t(0); k < v.size(); k += std::size_t(1)) sum += int64_t(v[k]) * (int64_t(k) + 1);\n  return int64_t(v.size()) * 1000 + sum;\n}",
                   expected=lambda i: 4 * 1000 + sum((i * 10 + k) * (k + 1) for k in range(4))),
}

# Each operation builds three elements 0,1,2 in a container, applies an
# operation, and returns the checksum of the final elements in order; the
# expected order is computed here.
OPS = ["build", "exchange", "swap", "replace", "return", "byvalue", "nested", "clear_refill"]


def container_ops(container, etype, op):
    """Return (helpers, body) computing `got` and `expected order` list."""
    E = etype
    if container == "vector":
        build = f"""  mut std::vector<{E}> items{{}};
  items.push_back(make_e(0));
  items.push_back(make_e(1));
  items.push_back(make_e(2));"""
        total = """  mut int64_t got = 0;
  for (mut std::size_t k = std::size_t(0); k < items.size(); k += std::size_t(1)) got = got * 3 + check_e(items[k]);"""
        if op == "build":
            return "", build + "\n" + total, [0, 1, 2]
        if op == "exchange":
            return "", build + f"""
  {E} taken = std::exchange(items[std::size_t(1)], make_e(5));
  mut int64_t got = 0;
  for (mut std::size_t k = std::size_t(0); k < items.size(); k += std::size_t(1)) got = got * 3 + check_e(items[k]);
  mut {E} kept = std::move(taken);
  got = got * 3 + check_e(kept);""", [0, 5, 2, 1]
        if op == "swap":
            return "", build + "\n  std::swap(items[std::size_t(0)], items[std::size_t(2)]);\n" + total, [2, 1, 0]
        if op == "replace":
            return "", build + "\n  items[std::size_t(1)] = make_e(7);\n" + total, [0, 7, 2]
        if op == "return":
            return f"""std::vector<{E}> make_items() {{
{build}
  return items;
}}
""", "  mut std::vector<" + E + "> items = make_items();\n" + total, [0, 1, 2]
        if op == "byvalue":
            return f"""int64_t sum_items(std::vector<{E}> items) {{
{total}
  return got;
}}
""", build + "\n  int64_t got = sum_items(std::move(items));", [0, 1, 2]
        if op == "nested":
            return "", build + f"""
  mut std::vector<std::vector<{E}>> outer{{}};
  outer.push_back(std::move(items));
  mut std::vector<{E}> back = std::move(outer[std::size_t(0)]);
  mut int64_t got = 0;
  for (mut std::size_t k = std::size_t(0); k < back.size(); k += std::size_t(1)) got = got * 3 + check_e(back[k]);""", [0, 1, 2]
        if op == "clear_refill":
            return "", build + "\n  items.clear();\n  items.push_back(make_e(3));\n  items.push_back(make_e(4));\n" + total, [3, 4]
    if container == "array":
        build = f"""  mut {E}[3] items = {{make_e(0), make_e(1), make_e(2)}};"""
        total = """  mut int64_t got = 0;
  for (mut std::size_t k = std::size_t(0); k < std::size_t(3); k += std::size_t(1)) got = got * 3 + check_e(items[k]);"""
        if op == "build":
            return "", build + "\n" + total, [0, 1, 2]
        if op == "exchange":
            return "", build + f"""
  {E} taken = std::exchange(items[std::size_t(1)], make_e(5));
  mut int64_t got = 0;
  for (mut std::size_t k = std::size_t(0); k < std::size_t(3); k += std::size_t(1)) got = got * 3 + check_e(items[k]);
  mut {E} kept = std::move(taken);
  got = got * 3 + check_e(kept);""", [0, 5, 2, 1]
        if op == "swap":
            return "", build + "\n  std::swap(items[std::size_t(0)], items[std::size_t(2)]);\n" + total, [2, 1, 0]
        if op == "replace":
            return "", build + "\n  items[std::size_t(1)] = make_e(7);\n" + total, [0, 7, 2]
        if op == "return":
            return f"""{E}[3] make_items() {{
{build}
  return items;
}}
""", f"  mut {E}[3] items = make_items();\n" + total, [0, 1, 2]
        if op == "byvalue":
            return f"""int64_t sum_items({E}[3] items) {{
{total}
  return got;
}}
""", build + "\n  int64_t got = sum_items(std::move(items));", [0, 1, 2]
        return None
    if container == "box":
        build = f"""  mut Trio<{E}> items = Trio<{E}>(make_e(0), make_e(1), make_e(2));"""
        total = """  int64_t got = (check_e(items.a) * 3 + check_e(items.b)) * 3 + check_e(items.c);"""
        trio = """class Trio<T> {
public:
  mut T a;
  mut T b;
  mut T c;
  Trio(T a, T b, T c) : a(std::move(a)), b(std::move(b)), c(std::move(c)) {}
};
"""
        if op == "build":
            return trio, build + "\n" + total, [0, 1, 2]
        if op == "exchange":
            return trio, build + f"""
  {E} taken = std::exchange(items.b, make_e(5));
  mut int64_t got = (check_e(items.a) * 3 + check_e(items.b)) * 3 + check_e(items.c);
  mut {E} kept = std::move(taken);
  got = got * 3 + check_e(kept);""", [0, 5, 2, 1]
        if op == "swap":
            return trio, build + "\n  std::swap(items.a, items.c);\n" + total, [2, 1, 0]
        if op == "replace":
            return trio, build + "\n  items.b = make_e(7);\n" + total, [0, 7, 2]
        if op == "return":
            return trio + f"""Trio<{E}> make_items() {{
{build}
  return items;
}}
""", f"  mut Trio<{E}> items = make_items();\n" + total, [0, 1, 2]
        if op == "byvalue":
            return trio + f"""int64_t sum_items(Trio<{E}> items) {{
{total}
  return got;
}}
""", build + "\n  int64_t got = sum_items(std::move(items));", [0, 1, 2]
        return None
    return None


def program(element, container, op):
    spec = ELEMENTS[element]
    shaped = container_ops(container, spec["type"], op)
    if shaped is None:
        return None
    helpers, body, order = shaped
    expected = 0
    for index in order:
        expected = expected * 3 + spec["expected"](index)
    source = PRELUDE + "#include <std/utility>\n"
    if "helper" in spec:
        source += spec["helper"] + "\n"
    source += spec["check"] + "\n"
    source += f"{spec['type']} make_e(int32_t i) {{\n"
    cases = sorted(set([0, 1, 2, 3, 4, 5, 7]))
    for i in cases:
        source += f"  if (i == {i}) return {spec['make'](i)};\n"
    source += f"  return {spec['make'](9)};\n}}\n\n"
    source += helpers + "\n"
    source += f"""int32_t run() {{
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
    return source


def cells():
    for element, container, op in itertools.product(ELEMENTS, ["vector", "array", "box"], OPS):
        source = program(element, container, op)
        if source is not None:
            yield f"{element}__{container}__{op}", source
