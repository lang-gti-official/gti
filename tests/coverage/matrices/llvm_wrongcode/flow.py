"""Matrix 3: owned kind x control-flow ownership pattern, self-checking."""
import itertools

from .transfer import PRELUDE
from .containers import ELEMENTS


def pattern(name, E, X):
    """X(i) gives the expected checksum of make_e(i). Returns (helpers, body, expected)."""
    if name == "conditional_move":
        return "", f"""  mut int64_t got = 0;
  for (mut int32_t round = 0; round < 2; round += 1) {{
    mut {E} a = make_e(1);
    mut {E} sink = make_e(2);
    if (round == 1) {{
      sink = std::move(a);
    }} else {{
      got = got * 3 + check_e(a);
    }}
    got = got * 3 + check_e(sink);
  }}""", ((0 * 3 + X(1)) * 3 + X(2)) * 3 + X(1)
    if name == "loop_keep":
        return "", f"""  mut {E} keep = make_e(0);
  mut int64_t got = 0;
  for (mut int32_t i = 1; i < 5; i += 1) {{
    mut {E} x = make_e(i);
    got = got * 3 + check_e(x);
    if (i == 3) keep = std::move(x);
  }}
  got = got * 3 + check_e(keep);""", (((((0 * 3 + X(1)) * 3 + X(2)) * 3 + X(3)) * 3 + X(4)) * 3 + X(3))
    if name == "early_return":
        return f"""int64_t pick(int32_t which) {{
  mut {E} first = make_e(1);
  mut {E} second = make_e(2);
  if (which == 0) return check_e(first);
  {{
    mut {E} third = make_e(3);
    if (which == 1) return check_e(third) * 3 + check_e(second);
  }}
  return check_e(second);
}}
""", """  int64_t got = (pick(0) * 3 + pick(1)) * 3 + pick(2);""", ((X(1)) * 3 + (X(3) * 3 + X(2))) * 3 + X(2)
    if name == "switch_move":
        return f"""enum Carrier {{ some({E} item), nothing }};
""", f"""  mut int64_t got = 0;
  for (mut int32_t i = 0; i < 3; i += 1) {{
    mut Carrier carrier = i == 1 ? Carrier::nothing : Carrier::some(make_e(i));
    switch (carrier) {{
    case Carrier::some(auto& item): got = got * 3 + check_e(item); break;
    case Carrier::nothing: got = got * 3 + 1; break;
    }}
  }}""", ((X(0)) * 3 + 1) * 3 + X(2)
    if name == "nested_scope":
        return "", f"""  mut {E} outer = make_e(1);
  mut int64_t got = 0;
  {{
    mut {E} inner = std::move(outer);
    got = check_e(inner);
    {{
      mut {E} deeper = std::move(inner);
      got = got * 3 + check_e(deeper);
    }}
  }}
  outer = make_e(2);
  got = got * 3 + check_e(outer);""", (X(1) * 3 + X(1)) * 3 + X(2)
    if name == "break_continue":
        return "", f"""  mut int64_t got = 0;
  for (mut int32_t i = 0; i < 6; i += 1) {{
    mut {E} x = make_e(i);
    if (i == 1) continue;
    if (i == 4) break;
    got = got * 3 + check_e(x);
  }}""", ((X(0)) * 3 + X(2)) * 3 + X(3)
    if name == "replace_loop":
        return "", f"""  mut {E} slot = make_e(0);
  mut int64_t got = 0;
  for (mut int32_t i = 1; i < 4; i += 1) {{
    slot = make_e(i);
    got = got * 3 + check_e(slot);
  }}""", ((X(1)) * 3 + X(2)) * 3 + X(3)
    if name == "closure_loop":
        return "", f"""  mut int64_t got = 0;
  for (mut int32_t i = 0; i < 3; i += 1) {{
    mut {E} x = make_e(i);
    auto reader = [held = std::move(x)]() -> int64_t {{
      mut int64_t total = 0;
      total += check_e(held);
      return total;
    }};
    got = got * 3 + reader();
  }}""", ((X(0)) * 3 + X(1)) * 3 + X(2)
    if name == "while_drain":
        return "", f"""  mut std::vector<{E}> items{{}};
  for (mut int32_t i = 0; i < 4; i += 1) items.push_back(make_e(i));
  mut int64_t got = 0;
  mut std::size_t index = std::size_t(0);
  while (index < items.size()) {{
    mut {E} taken = std::exchange(items[index], make_e(7));
    got = got * 3 + check_e(taken);
    index += std::size_t(1);
  }}
  got = got * 3 + check_e(items[std::size_t(2)]);""", ((((X(0)) * 3 + X(1)) * 3 + X(2)) * 3 + X(3)) * 3 + X(7)
    raise ValueError(name)


PATTERNS = ["conditional_move", "loop_keep", "early_return", "switch_move",
            "nested_scope", "break_continue", "replace_loop", "closure_loop", "while_drain"]


def program(element, name):
    spec = ELEMENTS[element]
    helpers, body, expected = pattern(name, spec["type"], spec["expected"])
    source = PRELUDE + "import std.utility;\n"
    if "helper" in spec:
        source += spec["helper"] + "\n"
    source += spec["check"] + "\n"
    source += f"{spec['type']} make_e(int32_t i) {{\n"
    for i in [0, 1, 2, 3, 4, 5, 7]:
        source += f"  if (i == {i}) return {spec['make'](i)};\n"
    source += f"  return {spec['make'](9)};\n}}\n\n" + helpers + "\n"
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
    for element, name in itertools.product(ELEMENTS, PATTERNS):
        yield f"{element}__{name}", program(element, name)
