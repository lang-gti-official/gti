"""Matrix 18: cleanup order under control flow.

Objects at several scope depths meet early returns, break and continue in
single and nested loops, while loops, switch cases, branch-local objects and
condition temporaries. Each Mark reports its code when destroyed; the program
compares the sequence with the order GTI specifies (docs/language/
execution.md 4.2.3): locals end in reverse initialization order at every exit
from their scope, and a full-expression's temporaries end with it.
"""

PRELUDE = """import std.format;
import std.atomic;
std::atomic<int32_t> position{0};
std::atomic<int32_t> first_bad{-1};
int32_t expected_at(int32_t index) {
EXPECTED  return -1;
}
void note(int32_t code) {
  int32_t at = position.fetch_add(1);
  if (first_bad.load() < 0 && expected_at(at) != code) first_bad.store(at);
}
class Mark {
  int32_t code;
public:
  Mark(int32_t code) : code(code) {}
  Mark(Mark& other) = delete;
  Mark(Mark&& other) = default;
  ~Mark() { note(this.code); }
  int32_t value() { return this.code; }
};
enum Kind { small, large, none };
"""

SCENARIOS = {
    "early_return_nested": ("""int32_t leave(int32_t n) {
  Mark a = Mark(1);
  {
    Mark b = Mark(2);
    if (n > 0) {
      Mark c = Mark(3);
      return 7;
    }
    Mark d = Mark(4);
  }
  Mark e = Mark(5);
  return 8;
}
""", """  if (leave(1) != 7) return 3;
  note(90);
  if (leave(0) != 8) return 4;""", [3, 2, 1, 90, 4, 2, 5, 1]),
    "loop_break_continue": ("", """  for (mut int32_t i = 1; i <= 4; i += 1) {
    Mark m = Mark(i);
    if (i == 2) continue;
    if (i == 4) break;
    Mark n = Mark(i + 5);
  }
  note(90);""", [6, 1, 2, 8, 3, 4, 90]),
    "nested_loops": ("", """  Mark outer = Mark(9);
  for (mut int32_t i = 1; i <= 2; i += 1) {
    Mark row = Mark(i * 10);
    for (mut int32_t j = 1; j <= 3; j += 1) {
      Mark cell = Mark(i * 10 + j);
      if (j == 2) break;
    }
    note(80 + i);
  }
  note(90);""", [11, 12, 81, 10, 21, 22, 82, 20, 90, 9]),
    "while_return": ("""int32_t drain(int32_t limit) {
  Mark guard = Mark(1);
  mut int32_t i = 0;
  while (true) {
    Mark step = Mark(10 + i);
    if (i == limit) {
      Mark last = Mark(50);
      return i;
    }
    i += 1;
  }
}
""", """  if (drain(2) != 2) return 3;
  note(90);""", [10, 11, 50, 12, 1, 90]),
    "switch_return": ("""int32_t choose(Kind kind) {
  Mark a = Mark(1);
  switch (kind) {
  case Kind::small: {
    Mark s = Mark(2);
    return 10;
  }
  case Kind::large: {
    Mark l = Mark(3);
    break;
  }
  case Kind::none:
    return 30;
  }
  Mark after = Mark(4);
  return 20;
}
""", """  if (choose(Kind::small) != 10) return 3;
  note(90);
  if (choose(Kind::large) != 20) return 4;
  note(91);
  if (choose(Kind::none) != 30) return 5;""", [2, 1, 90, 3, 4, 1, 91, 1]),
    "branch_locals": ("", """  for (mut int32_t i = 0; i < 2; i += 1) {
    Mark head = Mark(1);
    if (i == 0) {
      Mark left = Mark(2);
      note(80);
    } else {
      Mark right = Mark(3);
      note(81);
    }
    note(82);
  }
  note(90);""", [80, 2, 82, 1, 81, 3, 82, 1, 90]),
    "condition_temporaries": ("", """  if (Mark(3).value() > 2) {
    Mark body = Mark(4);
    note(80);
  }
  mut int32_t i = 0;
  while (Mark(5 + i).value() < 7) {
    note(81);
    i += 1;
  }
  note(90);""", [3, 80, 4, 5, 81, 6, 81, 7, 90]),
    "return_local": ("""Mark build(int32_t n) {
  Mark keep = Mark(n);
  Mark drop = Mark(n + 1);
  if (n > 5) {
    Mark extra = Mark(n + 2);
    return keep;
  }
  return keep;
}
""", """  {
    Mark first = build(1);
    note(80);
    Mark second = build(7);
    note(81);
  }
  note(90);""", [2, 80, 9, 8, 81, 7, 1, 90]),
}


def program(scenario):
    helpers, body, expected = SCENARIOS[scenario]
    table = "".join(f"  if (index == {i}) return {code};\n" for i, code in enumerate(expected))
    return PRELUDE.replace("EXPECTED", table) + helpers + f"""int32_t run() {{
{body}
  return 0;
}}

int main() {{
  int32_t status = run();
  if (status != 0) return status;
  if (first_bad.load() >= 0 || position.load() != {len(expected)}) {{
    std::println("order {{}} {{}}", first_bad.load(), position.load());
    return 2;
  }}
  std::println("ok");
  return 0;
}}
"""


def cells():
    for scenario in SCENARIOS:
        yield scenario, program(scenario)
