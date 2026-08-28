#!/usr/bin/env python3

"""Returning a read-only reference parameter from a free function.

`int32_t& pick(int32_t& candidate) { return candidate; }` is the one shape
GTI accepts for an argument-derived borrowed return: exactly one read-only
reference parameter provides the dependency. MIR lowering used to reclassify
that parameter's seeded entry loan in place as the escaping Return loan.
An entry loan has no producing instruction, so nothing ever named the address
it published and the emitted callee returned a null pointer -- every caller
that read through the result crashed. Lowering now publishes a fresh Return
loan from the entry loan and retires the entry loan at the same instruction
boundary, as the by-value carrier and nested-call returns already did.

Each program must build and run correctly at O0 and O3 for c++20 and c++23,
and each returns nonzero from `main` when a value read through the returned
reference is wrong.
"""

import pathlib
import subprocess
import sys
import tempfile


PRELUDE = """\
class Box {
  mut int32_t value;

public:
  Box(int32_t initial) : value(initial) {}
  mut int32_t& write() mut { return this.value; }
  int32_t& observe() { return this.value; }
  int32_t read() { return this.value; }
  void store(int32_t next) mut { this.value = next; }
};

class Boxes {
public:
  mut Box left = Box(1);
  mut Box right = Box(2);
};

class Pair {
public:
  Pair() {}
  mut int32_t first = 3;
  mut int32_t second = 4;
};

int32_t& pick(int32_t& candidate) { return candidate; }
"""


CASES = (
    (
        # The reported shape: a local read-only reference binding whose
        # initializer is a free function returning its argument.
        "retain-argument-return",
        """\
int main() {
  mut int32_t v = 0;
  v = 7;
  int32_t& chosen = pick(v);
  if (chosen != 7) {
    return 1;
  }
  return 0;
}
""",
    ),
    (
        # No binding at all: the returned reference is read inside the same
        # full expression. This also crashed, so the defect was never about
        # the binding.
        "argument-return-unbound",
        """\
int main() {
  mut int32_t v = 0;
  v = 41;
  if (pick(v) != 41) {
    return 1;
  }
  return 0;
}
""",
    ),
    (
        # The binding outlives the full expression that produced it.
        "retain-across-statements",
        """\
int main() {
  mut int32_t v = 0;
  v = 12;
  int32_t& chosen = pick(v);
  int32_t other = 30;
  if (chosen + other != 42) {
    return 1;
  }
  if (chosen != 12) {
    return 2;
  }
  return 0;
}
""",
    ),
    (
        # The argument is a field of a live object, and the object is mutated
        # between two separate borrows: the returned reference must designate
        # the caller's storage, not a copy of it.
        "argument-return-field",
        """\
int main() {
  mut Pair pair = Pair();
  pair.first = 11;
  {
    int32_t& borrowed = pick(pair.first);
    if (borrowed != 11) {
      return 1;
    }
  }
  pair.first = 23;
  {
    int32_t& borrowed = pick(pair.first);
    if (borrowed != 23) {
      return 2;
    }
  }
  if (pair.second != 4) {
    return 3;
  }
  return 0;
}
""",
    ),
    (
        # The argument is itself a borrowed call result, so the caller stages
        # a call-result loan before the argument-derived return consumes it.
        "argument-return-call-result",
        """\
int main() {
  mut Box box = Box(0);
  box.store(19);
  int32_t& chosen = pick(box.observe());
  if (chosen != 19) {
    return 1;
  }
  if (box.read() != 19) {
    return 2;
  }
  return 0;
}
""",
    ),
    (
        # The parameter is forwarded through a second argument-returning free
        # function, so the outer body returns a call-result chain rooted at
        # its own entry loan.
        "argument-return-forwarded",
        """\
int32_t& forward(int32_t& candidate) { return pick(candidate); }

int main() {
  mut int32_t v = 0;
  v = 5;
  int32_t& chosen = forward(v);
  if (chosen != 5) {
    return 1;
  }
  return 0;
}
""",
    ),
    (
        # A class-typed reference parameter returned by the same shape: the
        # published reference must still reach the caller's object.
        "argument-return-class",
        """\
Box& identity(Box& candidate) { return candidate; }

int main() {
  mut Box box = Box(0);
  box.store(31);
  {
    Box& alias = identity(box);
    if (alias.read() != 31) {
      return 1;
    }
  }
  box.store(32);
  {
    Box& alias = identity(box);
    if (alias.read() != 32) {
      return 2;
    }
  }
  return 0;
}
""",
    ),
    (
        # Working contrasts that must keep working: a receiver-derived method
        # return, read-only and mutable, retained in a local binding.
        "receiver-return-contrast",
        """\
int main() {
  mut Boxes parent = Boxes();
  {
    mut int32_t& child = parent.left.write();
    child = 15;
  }
  {
    int32_t& observed = parent.left.observe();
    if (observed != 15) {
      return 1;
    }
  }
  if (parent.right.read() != 2) {
    return 2;
  }
  return 0;
}
""",
    ),
)


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: argument_reference_return_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-argument-return-") as temporary:
        root = pathlib.Path(temporary)
        for label, body in CASES:
            source = root / f"{label}.gti"
            source.write_text(PRELUDE + "\n" + body, encoding="utf8")
            for standard in ("c++20", "c++23"):
                for optimization in ("O0", "O3"):
                    mode = f"{label}-{standard}-{optimization}"
                    executable = root / mode
                    built = run(
                        [
                            str(compiler),
                            str(source),
                            "--std",
                            standard,
                            f"-{optimization}",
                            "-o",
                            str(executable),
                        ]
                    )
                    if built.returncode != 0:
                        return fail(f"build {mode}", built)

                    executed = run([str(executable)])
                    if (
                        executed.returncode != 0
                        or executed.stdout != ""
                        or executed.stderr != ""
                    ):
                        return fail(f"run {mode}", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
