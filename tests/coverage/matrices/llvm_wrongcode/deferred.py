"""Matrix: deferred actions under early exit, loops, propagation and failure.

GTI has no `defer` statement; deferral is written as a source-defined scope
guard, an object whose destructor runs a stored callable. Its timing is
therefore the destruction contract (docs/language/execution.md 4.2.3, 4.8,
4.10): guards end in reverse successful-initialization order at every exit
from their scope (fallthrough, `return`, `break`, `continue`), a `?` error
edge cleans only the guards created before it, and a defined failure runs
the same cleanup before the hosted boundary exits with status 70. A guard's
own fields (the callable and its captures) are destroyed after its
destructor body, and a moved guard runs exactly once, at its final owner.

Each run prints a trace to stdout; the expected trace is derived in Python
from these rules: a guard event `gN` prints N, and for a kind whose callable
owns a Mark it is followed by the Mark's destruction (9). A dismissed guard
prints only its capture's destruction.

Axes: guard kind (generic `Defer<F>` from a factory, erased
`std::move_only_function`, either holding an owned capture) x exit
(fallthrough, nested early return, loop break/continue, `?` propagation,
dismissal, failure). Fixed scenarios cover a stack of deferred callbacks
run in reverse, a guard returned from a function, a guard field of a class,
and a guard whose closure borrows a local.

Excluded shapes: a `defer` statement and `[[deferred]]` (not in the
language; the attribute is rejected with migration guidance).
"""

PRELUDE = """import std.format;
import std.vector;
import std.functional;
import std.utility;
import std.atomic;
import std.panic;
std::atomic<int32_t> unit{1};
int32_t one() { return unit.load(); }
void note(int32_t code) { std::println("{}", code); }
class Mark {
  int32_t code;
public:
  Mark(int32_t code) : code(code) {}
  Mark(Mark& other) = delete;
  Mark(Mark&& other) = default;
  ~Mark() { note(9); }
  int32_t value() { return this.code; }
};
class Defer<F> {
  F action;
  mut bool armed;
public:
  Defer(F action) : action(std::move(action)), armed(true) {}
  Defer(Defer<F>& other) = delete;
  Defer(Defer<F>&& other) = default;
  void dismiss() mut { this.armed = false; }
  ~Defer() { if (this.armed) this.action(); }
};
Defer<F> defer<F>(F action) { return Defer<F>(std::move(action)); }
class Guard {
  std::move_only_function<void()> action;
  mut bool armed;
public:
  Guard(std::move_only_function<void()> action) : action(std::move(action)), armed(true) {}
  Guard(Guard& other) = delete;
  Guard(Guard&& other) = default;
  void dismiss() mut { this.armed = false; }
  ~Guard() { if (this.armed) this.action(); }
};
"""

# kind: (declaration with NAME and CODE, whether the callable owns a Mark)
KINDS = {
    "generic": ("int32_t NAME_code = CODE; "
                "mut auto NAME = defer([NAME_code]() -> void { note(NAME_code); });", False),
    "generic_owned": ("Mark NAME_mark = Mark(CODE); "
                      "mut auto NAME = defer([m = std::move(NAME_mark)]() -> void { note(m.value()); });",
                      True),
    "erased": ("int32_t NAME_code = CODE; mut Guard NAME = Guard(std::move_only_function<void()>("
               "[NAME_code]() -> void { note(NAME_code); }));", False),
    "erased_owned": ("Mark NAME_mark = Mark(CODE); mut Guard NAME = Guard(std::move_only_function<void()>("
                     "[m = std::move(NAME_mark)]() -> void { note(m.value()); }));", True),
}

# exit: (helpers + run() using GUARD(name, code), trace, status)
EXITS = {
    "fallthrough": ("""int32_t run() {
  GUARD(a, 1)
  {
    GUARD(b, 2)
    note(50);
  }
  GUARD(c, 3)
  note(51);
  return 0;
}
""", [50, "g2", 51, "g3", "g1"], 0),
    "early_return": ("""int32_t leave(int32_t n) {
  GUARD(a, 1)
  {
    GUARD(b, 2)
    if (n > 0) {
      GUARD(c, 3)
      return 7;
    }
  }
  GUARD(d, 4)
  return 8;
}
int32_t run() {
  if (leave(1) != 7) return 1;
  note(90);
  if (leave(0) != 8) return 2;
  return 0;
}
""", ["g3", "g2", "g1", 90, "g2", "g4", "g1"], 0),
    "loop_exits": ("""int32_t run() {
  for (mut int32_t i = 1; i <= 4; i += 1) {
    GUARD(g, i)
    if (i == 2) continue;
    if (i == 4) break;
    note(10 + i);
  }
  note(90);
  return 0;
}
""", [11, "g1", "g2", 13, "g3", "g4", 90], 0),
    "propagation": ("""expected<int32_t, int32_t> check(int32_t raw) {
  if (raw < 0) return unexpected(raw);
  return raw * 2;
}
expected<int32_t, int32_t> step(int32_t raw) {
  GUARD(a, 1)
  int32_t value = ?check(raw);
  GUARD(b, 2)
  note(value);
  return value;
}
int32_t run() {
  auto good = step(3);
  if (!good || good.value() != 6) return 1;
  note(90);
  auto bad = step(-4);
  if (bad || bad.error() != -4) return 2;
  return 0;
}
""", [6, "g2", "g1", 90, "g1"], 0),
    "dismissed": ("""int32_t run() {
  GUARD(a, 1)
  GUARD(b, 2)
  {
    GUARD(c, 3)
    c.dismiss();
    note(50);
  }
  b.dismiss();
  note(51);
  return 0;
}
""", [50, "x", 51, "x", "g1"], 0),
    "failure": ("""int32_t run() {
  GUARD(a, 1)
  {
    GUARD(b, 2)
    note(50);
    if (one() == 1) std::panic("deferred failure");
    GUARD(c, 3)
  }
  return 0;
}
""", [50, "g2", "g1"], 70),
}

SCENARIOS = {
    # Deferred callbacks pushed on a stack run newest first when it ends.
    "callback_stack": ("""class DeferStack {
  mut std::vector<std::move_only_function<void()>> actions;
public:
  DeferStack() : actions(std::vector<std::move_only_function<void()>>()) {}
  void push(std::move_only_function<void()> action) mut { this.actions.push_back(std::move(action)); }
  ~DeferStack() {
    mut std::size_t i = this.actions.size();
    while (i > std::size_t(0)) {
      i -= std::size_t(1);
      this.actions[i]();
    }
  }
};
int32_t run() {
  {
    mut DeferStack stack = DeferStack();
    for (mut int32_t i = 1; i <= 3; i += 1) {
      stack.push(std::move_only_function<void()>([i]() -> void { note(i); }));
    }
    note(50);
  }
  note(51);
  return 0;
}
""", [50, 3, 2, 1, 51], 0),
    # A guard moved out of its function runs once, at its final owner.
    "returned_guard": ("""Guard arm(int32_t code) {
  mut Guard local = Guard(std::move_only_function<void()>([code]() -> void { note(code); }));
  note(40);
  return local;
}
int32_t run() {
  {
    Guard held = arm(4);
    note(50);
  }
  note(51);
  return 0;
}
""", [40, 50, 4, 51], 0),
    # A guard field runs after the owner's destructor body.
    "guard_field": ("""class Owner {
  Guard guard;
public:
  Owner(int32_t code) : guard(Guard(std::move_only_function<void()>([code]() -> void { note(code); }))) {}
  ~Owner() { note(70); }
};
int32_t run() {
  {
    Owner first = Owner(1);
    Owner second = Owner(2);
  }
  note(51);
  return 0;
}
""", [70, 2, 70, 1, 51], 0),
    # A deferred closure that reads a local through a reference capture.
    "borrowing_capture": ("""int32_t run() {
  mut int32_t local = 5;
  local = 2;
  {
    auto guard = defer([&local]() -> void { note(local); });
    note(50);
  }
  note(51);
  return 0;
}
""", [50, 2, 51], 0),
}

MAIN = """int main() {
  int32_t status = run();
  if (status != 0) {
    std::println("status {}", status);
    return status;
  }
  std::println("ok");
  return 0;
}
"""


def expand(trace, owned):
    lines = []
    for event in trace:
        if isinstance(event, int):
            lines.append(event)
        elif event == "x":
            lines.extend([9] if owned else [])
        else:
            lines.extend([int(event[1:]), 9] if owned else [int(event[1:])])
    return lines


def expectation(lines, status):
    stdout = "".join(f"{line}\n" for line in lines)
    if status == 0:
        return {"stdout": stdout + "ok\n"}
    return {"stdout": stdout, "returncode": 70,
            "stderr": 'runtime error[GTI-R0017]: panic: "deferred failure"'}


def program(kind, body):
    declaration, _ = KINDS[kind]
    lines = []
    for line in body.splitlines():
        stripped = line.strip()
        if stripped.startswith("GUARD("):
            name, code = stripped[len("GUARD("):-1].split(", ")
            indent = line[:len(line) - len(line.lstrip())]
            line = indent + declaration.replace("NAME", name).replace("CODE", code)
        lines.append(line)
    return PRELUDE + "\n".join(lines) + "\n" + MAIN


def cells():
    for kind, (_, owned) in KINDS.items():
        for exit_, (body, trace, status) in EXITS.items():
            yield (f"{kind}_{exit_}", program(kind, body),
                   expectation(expand(trace, owned), status))
    for name, (body, trace, status) in SCENARIOS.items():
        yield name, PRELUDE + body + MAIN, expectation(trace, status)
